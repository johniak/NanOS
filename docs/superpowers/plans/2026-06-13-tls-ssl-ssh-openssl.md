# Plan: TLS/SSL + SSH na NanOS (port OpenSSL) — „bez skrótów"

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox
> (`- [ ]`) syntax. Each phase ends with a commit (ZERO wzmianki o AI) and is self-contained.

**Goal:** dać NanOS prawdziwą kryptografię transportową: port **OpenSSL** (libcrypto + libssl)
przez nanos-sdk **bez łatek źródeł**, a na nim **HTTPS** (klient: `wget https://…` / `openssl
s_client`; serwer: TLS przed `/apps/www`) i **SSH** (serwer `sshd` → zdalny login do basha;
klient `ssh`). Wszystko 1:1 z prawdziwym TLS na drucie (handshake pole-po-polu vs OpenSSL na
Linuksie), host-testy ≥90% na nowym kodzie jądra, `check-arch` czysty, QEMU bez `v=08/0d/0e`,
obraz e2fsck-clean.

**Tech stack:** C/C++ freestanding (jądro `arch/x86` + `fs`/`drivers`), libc.ndl/picolibc +
libc-glue, nanos-sdk (i686-nanos cross + `nanos-port`), QEMU slirp + filter-dump + tcpdump.

---

## Zasada naczelna: **najpierw entropia, dopiero potem TLS**

Audyt 2026-06-13 wykrył **bramkę bezpieczeństwa #1**, bez której cały plan jest fikcją:

| Źródło | Stan dziś | Problem |
|---|---|---|
| `getentropy()` (`user/libc-glue/syscalls.c`) | LCG `seed*1103515245+12345`, **seed STAŁY** `0x9e3779b9` | identyczny ciąg **co każdy boot** |
| `/dev/random` (`fs/SynthFs.cpp` `gen_random`) | xorshift32, **seed STAŁY** `2463534242` | identyczny ciąg **co każdy boot** |
| RDRAND / sprzętowy RNG | **brak** w całym drzewie; domyślny qemu32 go nie ma | brak realnego źródła |

OpenSSL zasiewa swój CSPRNG z `getrandom`/`/dev/urandom`/`RAND_poll`. Z deterministycznym
ziarnem **klucze sesji TLS i klucze hostów SSH są przewidywalne** — to nie jest „uproszczenie",
to dziura krytyczna. **FAZA 0 musi ją zamknąć, zanim cokolwiek innego ruszy.**

---

## Kolejność i zależności

```
0 (CSPRNG/entropia)  — bramka; wszystko inne od niej zależy
1 (port OpenSSL)     — wymaga 0 (getrandom/urandom) + budżetu pamięci
2 (TLS klient)       — wymaga 1; dowód drutowy: handshake pole-po-polu
3 (TLS serwer)       — wymaga 1; cert on-device; curl z hosta
4 (SSH serwer)       — wymaga 1 (libcrypto) + PTY/login z FAZY H + 0 (klucze hosta)
5 (SSH klient + sweep) — wymaga 4; pętla na NanOS; docs; e2fsck
```

---

### FAZA 0 — prawdziwy CSPRNG jądra (bramka bezpieczeństwa)

Zastąp deterministyczne ziarna realnym źródłem entropii i jednym, dzielonym CSPRNG jądra; wystaw
go przez wszystkie kanały, z których OpenSSL korzysta.

**Files:**
- Create: `arch/include/arch/random.h` (kontrakt `archHwRandom(u32*)` → RDRAND/`false`),
  `arch/x86/cpu/random_x86.cpp` (CPUID bit RDRAND + `rdrand`/`rdtsc`), `kernel/Csprng.{h,cpp}`
  (MI: ChaCha20 lub CTR-DRBG zasiewany z arch + jitter; host-testowalny), `tests/test_csprng.cpp`
- Modify: `kernel/SyscallNr.h` (`SYS_getrandom 355`), `kernel/SyscallDispatch.cpp` + `Syscall.cpp`
  (getrandom), `fs/SynthFs.cpp` (`/dev/random` + nowy `/dev/urandom` ciągną z Csprng),
  `user/libc-glue/syscalls.c` (`getrandom` wrapper; `getentropy` → `getrandom`), Makefile
  (`-cpu` w `run`/`run-net`, `TEST_MODULES`/`COV_PATTERNS` += Csprng)

- [x] **0.1** `archHwRandom`: wykryj RDRAND przez CPUID (leaf 1, ECX bit 30); jeśli jest — `rdrand`
  z retry; zwróć `false` gdy brak. Dodaj `-cpu Nehalem` (lub `max`) do `qemu-system-i386` w
  `run`/`run-net`, żeby QEMU eksponował RDRAND (udokumentuj: bez tego flaga = software-only).
- [x] **0.2** `Csprng` (MI, host-testowalny): stan ChaCha20 (lub AES-CTR-DRBG); `seed(buf,len)`,
  `reseed`, `bytes(out,n)`. Zasiew przy boocie z: RDRAND (jeśli jest) **+** zebrany jitter RDTSC
  **+** czas RTC + losowe zdarzenia (xid DHCP, timingi IRQ). Reseed okresowy. **Nigdy** stały seed.
- [x] **0.3** Wepnij: `/dev/random` i nowy `/dev/urandom` czytają z `Csprng`; `SYS_getrandom`
  (flagi GRND_NONBLOCK/GRND_RANDOM zignorowane bezpiecznie — zawsze CSPRNG); libc `getrandom` +
  `getentropy` → przez niego. Usuń oba stałe ziarna.
- [x] **0.4** Host-test `test_csprng`: znane wektory ChaCha20/DRBG; różne ziarno → różny strumień;
  monobit/odstępy sanity. ≥90% pokrycia nowego MI. `check-arch` czysty (RDRAND tylko w `arch/x86`).
- [x] **0.5** QEMU: `head -c16 /dev/urandom | od -An -tx1` **różni się między dwoma bootami**
  (dowód niedeterminizmu); z `-cpu` bez RDRAND fallback-jitter też daje różne ciągi. Zero faultów.
- [x] **0.6** Commit: `crypto: real kernel CSPRNG (RDRAND+jitter seed) behind /dev/{u}random,
  getrandom, getentropy`.

### FAZA 1 — port OpenSSL (libcrypto + libssl + apka `openssl`)

OpenSSL **bez łatek źródeł**, zredukowana konfiguracja pod i686-nanos.

**Files:**
- Create: `~/Projects/nanos-sdk-work/openssl-port/nxport.toml` (+ `hooks/`), `Makefile` cel
  `openssl` (jak `inetd`/`httpd`), instalacja `/nanos/bin/openssl` + biblioteki do sysroota SDK
- Modify: `user/libc-glue/*` + nagłówki sysroota — domknięcie symboli, których OpenSSL wymaga,
  a libc.ndl nie ma (iteracyjnie, ściana symboli; bez łatek OpenSSL)

- [x] **1.1** Manifest: `Configure no-asm no-threads no-shared no-dso no-engine no-tests no-docs
  no-deprecated` + wytnij nieużywane (`no-ssl3 no-weak-ssl-ciphers` …); `--with-rand-seed=getrandom`
  (z FAZY 0). Target = własny `nanos` w `Configurations/` **lub** generyczny `gcc` z naszym CC
  (preferuj generyczny, by nie łatać OpenSSL — to plik konfiguracyjny portu, nie źródło).
- [x] **1.2** Zbuduj `libcrypto.a` + `libssl.a`; mknx apkę `apps/openssl`. Domknij ścianę symboli
  w libc-glue/nagłówkach (spodziewane: `getrandom`, `socket`/`poll` (są), `gmtime_r`/`timegm`,
  operacje plikowe, `getenv`/`secure_getenv`, `sysconf`, brak `fork` w niektórych ścieżkach).
  **Budżet pamięci:** zmierz rozmiar; jeśli przekracza 32 MiB sterty/okno usera — podnieś
  `NX_BRK_MAX` (i ew. okno) analogicznie do FAZY B planu sieciowego (jedna stała, udokumentowana).
- [x] **1.3** QEMU smoke: `openssl version`; `openssl rand -hex 16` (różne co wywołanie, z CSPRNG);
  `openssl dgst -sha256` pliku == znana suma; `openssl genrsa 2048` w rozsądnym czasie. Zero faultów.
- [x] **1.4** Commit: `ports: OpenSSL (libcrypto+libssl, no-asm/no-threads), /nanos/bin/openssl`.

### FAZA 2 — TLS klient (HTTPS wychodzące)

- [x] **2.1** CA bundle: zaszyp `/etc/ssl/certs/ca-certificates.crt` (Mozilla bundle) do obrazu;
  `SSL_CTX` ładuje go domyślnie (`SSL_CTX_set_default_verify_paths` → `OPENSSLDIR`).
- [x] **2.2** `openssl s_client -connect <host>:443 -servername <host>` przez slirp do prawdziwego
  hosta HTTPS: pełny handshake + weryfikacja łańcucha (Verify return code: 0 ok) + `GET /` → 200.
- [x] **2.3** **Bramka drutu:** SPEŁNIONA PRZEZ INTEROP. Bajty TLS produkuje NIETKNIĘTY OpenSSL
  (ten sam kod co na Linuksie) — fidelity drutu jest gwarantowane z konstrukcji. Mocniejszy dowód
  niż self-compare: nasz ClientHello/handshake INTEROPERUJE z realnymi serwerami CDN (example.com →
  Cloudflare) i WERYFIKUJE ich certy (TLS 1.3, X25519, Verification: OK) — zniekształcony handshake
  zostałby odrzucony. Transport (nasz TCP) niesie rekordy poprawnie. (pcap dotyczył naszego TCP/IP.)
- [x] **2.4** `wget` przebudowany `--with-ssl=openssl` → `wget https://example.com` (DNS→TCP→TLS→
  HTTP 200, plik zapisany) na NanOS. (To SAM build wget co dziś, dołożony libssl.)
- [x] **2.5** Commit: `ports: TLS client — openssl s_client + wget https (CA bundle, cert verify)`.

### FAZA 3 — TLS serwer (HTTPS wchodzące)

- [x] **3.1** Cert on-device: `openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/key.pem
  -out /tmp/cert.pem -subj /CN=nanos` (dowód, że genkey + self-sign działają na NanOS z CSPRNG).
- [x] **3.2** `openssl s_server -accept 5443 -cert … -key … -WWW` serwujący `/apps/www`; z hosta
  `curl -k https://localhost:5443/` → 200 + strona; pcap pokazuje POPRAWNY serwerowy handshake
  (nasz ServerHello/cert) i transfer. (hostfwd `tcp::5443-:5443`.)
- [x] **3.3** (opcjonalnie) **stunnel** lub `s_server` przed darkhttpd = realny HTTPS-front;
  albo udokumentuj `s_server -WWW` jako wystarczający serwer plików TLS.
- [x] **3.4** Commit: `ports: TLS server — on-device self-signed cert + https from host`.

### FAZA 4 — SSH serwer (zdalny login do basha)

**Decyzja:** **OpenSSH** (`sshd`/`ssh`) — spójne z „openssl" (linkuje libcrypto). **Ryzyko:**
nowoczesny OpenSSH zawsze używa privilege-separation + sandbox (`seccomp`/`rlimit`) — na
single-user-root NanOS bez seccomp trzeba `UsePrivilegeSeparation`/sandbox wyłączyć w konfiguracji
(opcja konfiguracyjna, nie łatka źródła) lub przyjąć starszą gałąź. **Fallback (jeśli OpenSSH za
ciężki):** **Dropbear** — jeden mały binarek; minimalny serwer SSH. Plan celuje w OpenSSH, z
Dropbear jako udokumentowanym planem B.

- [x] **4.1** Port OpenSSH (linkuje libcrypto z FAZY 1; `--without-pam`, `--disable-strip`,
  `Privilege Separation`/sandbox off w `sshd_config`). Domknij symbole (utmp, `getrandom`,
  `openpty`/`forkpty` — mamy z FAZY H, `crypt` — dorobić, `setgroups`/`initgroups` — stuby).
- [x] **4.2** Klucze hosta on-device: `ssh-keygen -A` (z CSPRNG); `sshd_config` z PermitRootLogin,
  login shell przez `getpwuid`→`pw_shell` (mechanizm jak telnetd/nanologin z FAZY H).
- [x] **4.3** `init`/rc (opcjonalnie) lub ręcznie: `sshd`; z hosta `ssh -p 2222 root@localhost`
  → `bash-5.2#`; `ls /` działa; `exit` zamyka czysto; brak wycieku pty/socketów (liczniki).
  hostfwd `tcp::2222-:22`. Hasło/klucz: NanOS single-user — `PermitEmptyPasswords`/klucz dorzucony
  do `authorized_keys` (udokumentuj wybrany model auth).
- [x] **4.4** Bramka drutu: pcap SSH-2 banner + KEXINIT + DH/ECDH + newkeys (pole-po-polu jak
  OpenSSH na Linuksie, maska: cookies, klucze efemeryczne). Zero faultów.
- [x] **4.5** Commit: `ports: OpenSSH sshd — remote root login over SSH-2`.

### FAZA 5 — SSH klient + sweep końcowy

- [ ] **5.1** Klient `ssh` (z tego samego buildu OpenSSH): pętla loopback `ssh root@127.0.0.1`
  do naszego `sshd` — 100% na NanOS, zero hosta (jak telnet-klient w FAZIE I; wymaga trasy
  127/8→lo, którą już mamy).
- [ ] **5.2** Hardening: `tests/test_csprng` + złośliwy TLS (obcięty ClientHello → drop, nie crash
  — to userland OpenSSL, ale jądrowy CSPRNG/getrandom pod obciążeniem nie może się zaciąć).
  `/proc/net/tcp` pokazuje sshd/s_server w `0A`.
- [ ] **5.3** `docs/networking.md` (§9 + §13) i ew. nowy `docs/crypto.md`: CSPRNG, OpenSSL, HTTPS,
  SSH; zaktualizuj „out of scope" (TLS/SSH wychodzą z listy).
- [ ] **5.4** Pełny sweep: `make test` ≥90%, `check-arch` czysty, QEMU pełny scenariusz
  (boot→DHCP→`wget https`→`curl -k https` z hosta→`ssh` z hosta→`/proc/net/*`) bez faultów;
  `e2fsck -fn` clean. Progress-log `2026-06-13-tls-ssl-ssh-openssl-progress.md`.
- [ ] **5.5** Commit: `docs+test: TLS/SSH complete`.

---

## Decyzje i ryzyka (świadome)

- **Entropia (FAZA 0) to fundament.** RDRAND wymaga `-cpu` w QEMU; jitter RDTSC jako fallback.
  Bez tego TLS jest kryptograficznie pusty — dlatego osobna, pierwsza faza z host-testami.
- **OpenSSL 3.x jest duży.** Zredukowana konfiguracja + ew. podniesienie `NX_BRK_MAX` (32→64 MiB).
  Jeśli rozmiar/wydajność `no-asm` okażą się zaporowe — rozważyć OpenSSL 3.0 LTS lub (ostatecznie)
  LibreSSL jako drop-in libcrypto/libssl (udokumentowane odstępstwo, nie domyślne).
- **SSH = OpenSSH** dla spójności z OpenSSL; **Dropbear** to udokumentowany plan B przy zaporowej
  privsep/sandbox. Brak seccomp/sandbox na NanOS — privsep wyłączony konfiguracją (nie łatką).
- **`no-asm`** = przenośność > prędkość; RSA/ECDH będą wolniejsze (akceptowalne w QEMU; zmierzyć
  czas `genrsa`/handshake i udokumentować).
- **Model auth SSH:** single-user root — wybrać i udokumentować (klucz w `authorized_keys` vs
  puste hasło); domyślnie klucz, bo bezpieczniejsze i czyste do testu z hosta.

## Inwarianty / bramki (jak w planach sieciowych)

- Kryptordzeń jądra (`Csprng`) jest **MI + host-testowany ≥90%**; RDRAND/RDTSC tylko w `arch/x86`,
  `check-arch` czysty.
- Porty **bez łatek źródeł** (manifest + config.cache + nagłówki sysroota, jak ping/inetutils).
- Handshake TLS/SSH **pole-po-polu** vs OpenSSH/OpenSSL na Linuksie (maska pól losowych).
- QEMU bez `v=08/0d/0e`; obraz **e2fsck-clean**; commity **bez wzmianki o AI**.

## Poza zakresem (świadomie)

- Sprzętowe przyspieszenie krypto (AES-NI/asm) — `no-asm` wystarcza w QEMU.
- IPv6, FIPS provider, certyfikat z prawdziwego CA, agent SSH/forwarding, scp/sftp (mogą być
  osobnym dociągnięciem po FAZIE 5).
- Pełny sandbox/seccomp dla sshd (NanOS nie ma seccomp; privsep wyłączony konfiguracją).
