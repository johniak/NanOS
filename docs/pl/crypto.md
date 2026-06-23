# NanOS kryptografia: CSPRNG, TLS, SSH

NanOS posiada prawdziwy stos bezpieczeństwa transportowego: jądrowy CSPRNG, port OpenSSL (libcrypto +
libssl + CLI `openssl`), HTTPS jako klient i serwer, oraz serwer SSH-2. Ten plik dokumentuje, jak
poszczególne elementy się łączą; przepływ budowania/portowania opisuje `CLAUDE.md`, a plan/postęp
— `docs/superpowers/plans/2026-06-13-tls-ssl-ssh-openssl.md`.

## 1. Jądrowy CSPRNG (`kernel/Csprng.*`)

Wszystko, co potrzebuje nieprzewidywalnych bajtów, czerpie z JEDNEGO współdzielonego jądrowego
CSPRNG — nigdy ze stałego ziarna (wcześniejsze `/dev/random` i `getentropy` używały stałych ziarn,
więc każde uruchomienie dawało ten sam strumień, a wszelkie klucze z nich pochodne były
przewidywalne; to była bramka bezpieczeństwa, od której zależał cały stos).

- **Konstrukcja:** ChaCha20 w układzie „fast key erasure". Każde uzupełnienie uruchamia jeden blok
  ChaCha20 wg RFC 8439; pierwsze 32 bajty wyjścia nadpisują klucz (forward secrecy), a reszta jest
  wydawana. Funkcja blokowa jest weryfikowana względem wektora wzorcowego RFC 8439
  (`tests/test_csprng.cpp`); klasa jest niezależna od maszyny i testowana na hoście (≥90%).
- **Seedowanie:** przy starcie `csprngKernelSeed()` miesza RDRAND (gdy CPU go udostępnia — `make run`
  przekazuje `-cpu Nehalem`, więc QEMU go ma), złożony jitter czasowy RDTSC oraz epokę RTC.
  RDRAND/RDTSC żyją wyłącznie w `arch/x86/cpu/random_x86.cpp` za kontraktem `<arch/random.h>`
  (`check-arch` czysty). Dwa oddzielne uruchomienia dają RÓŻNE bajty — bramka niedeterminizmu.
- **Ekspozycja:** `/dev/random` + `/dev/urandom` (identyczne — CSPRNG nigdy się nie blokuje i jest
  zawsze zainicjalizowany), `getrandom(2)` (SYS_getrandom 355) oraz `getentropy(3)` — wszystkie
  czerpią z niego. OpenSSL zasiewa własny DRBG z `/dev/urandom` (`--with-rand-seed=devrandom`).
  `randhex` wypisuje oba kanały.

## 2. OpenSSL (`make openssl` -> /nanos/bin/openssl)

OpenSSL 3.0.15, niezmodyfikowany upstream, budowany krzyżowo przez nanos-sdk
(`no-asm/no-threads/no-shared`, stałobazowy ET_EXEC). libcrypto.a/libssl.a + nagłówki instalują się
do sysroot SDK, aby inne porty mogły je konsolidować. CLI działa na gościu: `openssl version`,
`rand -hex` (CSPRNG, różne przy każdym wywołaniu), `dgst -sha256` (bajt w bajt zgodny z hostem),
`genrsa 2048` (prawdziwy klucz na CSPRNG).

## 3. Klient TLS

- `openssl s_client -connect <host>:443` przez slirp NAT finalizuje prawdziwy handshake TLS 1.3 z
  żywym hostem internetowym (X25519, AES-256-GCM) i weryfikuje łańcuch względem dostarczonego
  pakietu CA Mozilla (`/nanos/ssl/cert.pem`, skompilowany OPENSSLDIR OpenSSL) — `Verification: OK`.
- `wget https://…` (zbudowany ponownie z `--with-ssl=openssl`): DNS -> TCP -> TLS (cert
  zweryfikowany) -> HTTP 200 -> plik zapisany na gościu.
- Bajty TLS są produkowane przez niezmodyfikowany OpenSSL, więc wierność transmisji jest inherentna;
  interoperacyjność z prawdziwymi serwerami CDN (które odrzuciłyby nieprawidłowy handshake) jest
  dowodem.

## 4. Serwer TLS

Gość samopodpisuje certyfikat na urządzeniu (`openssl req -x509 -newkey rsa:2048`, genrsa + podpis
X.509 na CSPRNG) i serwuje pliki przez `openssl s_server -accept 5443 -WWW`. Z hosta:
`curl -k https://localhost:5443/` zwraca stronę (pełny serwerowy TLS 1.3 do klienta hosta).

## 5. Serwer SSH (Dropbear)

`make dropbear` buduje Dropbear 2022.83 (serwer `dropbear` + `dropbearkey` + klient `dbclient`),
niezmodyfikowany upstream (konfiguracja przez `localoptions.h`). Wybrany zamiast OpenSSH, bo działa
jako root BEZ separacji uprawnień (bez chroot/setuid-do-nobody, którego wymaga współczesny OpenSSH)
i dostarcza własną kryptografię.

Uwierzytelnianie obsługuje zarówno **hasło**, jak i **klucz publiczny**:
- **Hasło** (bez zarządzania kluczami): obsługuje je `crypt(3)` NanOS (SHA-512 `$6$`, patrz wyżej);
  hash roota jest w `/nanos/config/passwd`. Z hosta: `ssh -p 2222 root@localhost` -> hasło `nanos`.
- **Klucz publiczny:** umieść klucz klienta w `~/.ssh/authorized_keys` (katalog domowy roota to
  `/disks/main/root`): `ssh -i key -p 2222 root@localhost`.

W obu przypadkach otrzymujesz sesję bash przez PTY jądra: baner SSH-2, KEX, klucz hosta ed25519,
uwierzytelnianie, powłoka — wszystko na gościu.

**sshd to usługa startowa:** `init` uruchamia dropbear razem z inetd (`start_sshd()` w
`user/init.c`), generując trwały klucz hosta ed25519 na dysku z odczytem/zapisem przy pierwszym
uruchomieniu. Więc po `make run` (które mapuje port 2222 hosta na port 22 gościa) wystarczy z hosta
`ssh -p 2222 root@localhost` — bez ręcznego startu. (Aby uruchomić ręcznie: `dropbearkey -t ed25519
-f /tmp/hk && dropbear -r /tmp/hk -p 22`.)

**Znane ograniczenie:** nieinteraktywne `ssh host cmd` (oraz sesja pty z flagą `-t`) zwraca poprawnie
wyjście polecenia, ale klient zawiesza się na końcu zamiast czysto zamknąć — teardown sesji Dropbear
(jego łańcuch SIGCHLD self-pipe -> select() -> reap -> channel-close) nie kończy się na NanOS.
Ścieżka auth/exec i wyjście nie są dotknięte; czysty teardown kanału jest zadaniem uzupełniającym w
ścieżce signal/select. (Sam pty raportuje teraz EOF przy zamknięciu slave'a — prawidłowa semantyka tty
— ale Dropbear zamyka się na budzeniu self-pipe, nie na EOF mastera.)

## Opcje budowania warte poznania

- Port korzystający z symboli danych libc (`stderr`/`stdout`) MUSI być zbudowany **bez PIC**
  (`-fno-pie -fno-PIC -no-pie`): domyślny PIC gcc kieruje symbole danych przez GOT, a mknx nie
  konwertuje referencji do danych GOT na importy `.nxe`, więc symbol rozwiązuje się do adresu 0 i
  powoduje błąd. (libc.ndl force-linkuje wszystkie trzy strumienie stdio, więc je eksportuje.)
- `-cpu Nehalem` udostępnia RDRAND; `-m 512` (teraz domyślne dla `make run`) — mapa pamięci umieszcza
  swoje okna powyżej RAM, więc RAM skaluje się swobodnie do ~1 GiB.
