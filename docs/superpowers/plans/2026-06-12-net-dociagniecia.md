# Plan: dociągnięcia sieci — „bardziej linuxowo i prawdziwie, nie podrutowane"

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** usunąć wszystkie prowizorki pozostałe po planie sieciowym (FAZY 0–14) i dociągnąć stos
do realnego Linuksa: prawdziwy DHCP zamiast statycznego IP, serwisy (inetd/telnetd/httpd — system
da się odwiedzić z zewnątrz), pełniejszy TCP na drucie, resolver czytający naprawdę `/etc`,
dostarczanie błędów ICMP do socketów, AF_UNIX/AF_PACKET zamiast EAFNOSUPPORT.

**Architecture:** te same inwarianty co plan bazowy — stos MI w `net/` (host-testowalny),
MD tylko w `arch/x86` i kextach; bramki bez zmian: host-testy ≥90%, pcap **pole-po-polu z maską
pól losowych** (NIE naiwne bajt-w-bajt), parytet strace, `check-arch` czysty, QEMU bez
`v=08/0d/0e`. Porty aplikacji przez nanos-sdk **bez łatek źródeł** (jak ping/wget).

**Tech Stack:** C++ freestanding (kernel `net/`), C (libc-glue/porty), doctest (host), QEMU
slirp + filter-dump + tcpdump/Scapy, nanos-sdk/nanos-port (busybox udhcpc, inetutils inetd/
telnetd/telnet/traceroute, darkhttpd).

> **Zasada naczelna (jak w planie bazowym): ZERO skrótów.** Każda faza poniżej zamienia
> konkretną prowizorkę na zachowanie 1:1 z Linuksem albo dodaje brakujący, prawdziwy element.
> Lista prowizorek wykrytych w audycie 2026-06-12 (każda ma fazę):
> 1. eth0 konfigurowane statycznie w kernelu zamiast DHCP (plan bazowy odroczył FAZĘ 10).
> 2. ZERO serwisów nasłuchujących — `listen`/`accept` działa, ale nic z niego nie korzysta.
> 3. `getservbyname`/`getprotobyname` czytają wbudowane tabele, a pliki `/etc/services`,
>    `/etc/protocols` LEŻĄ NA DYSKU i są ignorowane (`user/libc-glue/resolv.c:218-255`).
> 4. `openpty`/`forkpty`/`login_tty` = stuby ENOSYS (`user/libc-glue/posixstubs.c`), mimo że
>    kernel MA pełne PTY (nterm).
> 5. `SIOCADDRT` „handled minimally" (`kernel/Syscall.cpp:1142`) — nie parsuje prawdziwego
>    `struct rtentry`.
> 6. Przychodzące błędy ICMP (dest-unreachable/time-exceeded) NIE są dostarczane do socketów —
>    UDP `connect()`+`recv()` nigdy nie dostanie ECONNREFUSED; traceroute niemożliwy.
> 7. TCP bez delayed ACK, persist (zero-window probe), keepalive, window scaling, timestamps,
>    SACK — nasz SYN na drucie [mss only] jest odróżnialny od linuksowego [mss,sackOK,TS,WS].
> 8. Resolver: brak TCP/53 fallback (TC bit), search domains/ndots, wielu nameserverów,
>    reverse DNS (`gethostbyaddr` zwraca numeryczne — `resolv.c:207`).
> 9. AF_UNIX i AF_PACKET → EAFNOSUPPORT (tylko stałe w nagłówkach); `socketpair` → ENOSYS.
> 10. `docs/networking.md` zawiera 3 nieścisłości (AF_PACKET „partial", AF_UNIX „ENOSYS",
>     „byte-compare").
> 11. `TCB_N=4` współbieżnych połączeń TCP — wymuszone budżetem BSS <3 MiB; właściwa naprawa
>     (zanotowana w FAZIE 9 planu bazowego) to podniesienie bazy okna usera, nie przycinanie.

---

## Kolejność i zależności

```
A (docs)        — niezależna, natychmiast
B (pamięć)      — fundament pod C–H (większe pule, miejsce na nowe moduły)
C (ICMP→sock)   — wymaga B; odblokowuje traceroute (I)
D (TCP)         — wymaga B; niezależna od C
E (resolver/etc)— niezależna (userland)
F (AF_PACKET+DHCP) — wymaga B; domyka FAZĘ 10 planu bazowego
G (AF_UNIX)     — wymaga B; potrzebna serwisom z syslogiem (H opcjonalnie), socketpair
H (serwisy)     — wymaga D (serwer TCP pod obciążeniem), E (inetd czyta /etc/services),
                  realnego openpty (w tej fazie); telnetd potrzebuje PTY
I (narzędzia)   — wymaga C (traceroute), F (ifconfig na żywej konfiguracji DHCP)
J (sweep)       — na końcu: docs, /proc, hardening nowych powierzchni
```

Każda faza kończy się commitem (bez wzmianki o AI) i jest samodzielnie wartościowa.

---

### FAZA A — sprostowania w `docs/networking.md` (15 min, od ręki)

**Files:**
- Modify: `docs/networking.md`

- [x] **A1.** W tabeli §10 wiersz `AF_UNIX / AF_PACKET`: zamienić `AF_UNIX declared (ENOSYS),
  AF_PACKET partial` na `both: constants in headers only — socket() returns EAFNOSUPPORT
  (socketpair: ENOSYS)`. Stan faktyczny: `net/Socket.cpp:35` (EAFNOSUPPORT dla ≠AF_INET),
  `kernel/SyscallDispatch.cpp:135` (socketpair −38).
- [x] **A2.** W §12 zamienić „byte-compared" na „field-compared (with an explicit mask of
  legitimately random fields — IP ID, ISN, ephemeral ports, DNS ID; see the plan §2 gate)".
- [x] **A3.** W §5 i §13 dopisać odnośnik do tego planu jako „follow-up in progress".
- [x] **A4.** Commit: `docs: correct networking.md (AF_UNIX/AF_PACKET reality, field-compare wording)`.

### FAZA B — budżet pamięci: podnieść bazę okna usera, odbudować pule (fundament)

Prowizorka #11. FAZA 9 planu bazowego przycięła pule (NetBuf 128→96, TCB 8→4, bufory 8K→4K),
żeby kernel zmieścił się pod 0x400000, i zapisała: „as the kernel grows, raise the user window
base rather than trimming further". Fazy C–H POWIĘKSZĄ kernel — robimy to teraz, raz, porządnie.

**Files:**
- Modify: `user/nx.ld` (base 0x400000 → 0x800000), `user/libnanos.*`/`kernel/Exec.cpp`/
  `kernel/NxeLoader.cpp` (stałe load-base/stack-top), `arch/x86/mm/AddressSpace.cpp`
  (granica user-window), `net/NetBuf.cpp` (POOL_N 96→128), `net/Tcp.cpp` (TCB_N 4→16,
  SNDBUF/RCVBUF 4096→8192), `kernel/Syscall.cpp` (msg kbuf 4K→8K)
- Test: istniejące suity (to zmiana rozmiarów, nie logiki) + `tests/test_tcp.cpp` (TCB_N przez
  `tcpSlots()`)

- [x] **B1.** Zinwentaryzować WSZYSTKIE wystąpienia stałej 0x400000/0x500000 (grep po kernel/,
  user/, arch/, docs/): load base, stack top, granica user-window w AddressSpace, walidacje
  wskaźników user w Syscall. Wynik = checklista zmian (jedna stała w jednym miejscu per moduł).
- [x] **B2.** Podnieść bazę okna usera do **0x800000** (8 MiB): kernel ma wtedy ~5 MiB
  headroomu (dziś 3.03 MB kończy się @0x3e2f98). Stack top usera analogicznie (+0x100000 nad
  obrazem jak dziś). Wszystkie programy .nxe i .ndl są linkowane naszym `nx.ld` — wymaga
  PRZEBUDOWY całego userlandu i portów (`make _userland`, `make bash/grep/vim/ping/wget`),
  ale ZERO zmian źródeł.
- [x] **B3.** Odbudować pule: `POOL_N=128`, `TCB_N=16`, `SNDBUF/RCVBUF=8192`. Backlog zostaje
  64 (nadal < pool — punkt dropu się nie zmienia; zaktualizować komentarz w `NetBuf.cpp:10`).
- [x] **B4.** `make test` zielone; QEMU: boot + bash + `ping wp.pl` + `wget` (pełna regresja
  starych portów na nowej bazie), `readelf`/mapfile potwierdza koniec kernela < 0x800000 z
  zapasem; zero `v=08/0d/0e`.
- [x] **B5.** Zaktualizować `docs/networking.md` §11 (nowe wartości) + CLAUDE.md (mapa pamięci).
- [x] **B6.** Commit: `mm+net: raise user window to 0x800000, restore right-sized net pools`.

### FAZA C — dostarczanie błędów ICMP do socketów (jak Linux)

Prowizorka #6. Linux: ICMP error (typ 3/11/12) zawiera cytowany nagłówek IP+8B → jądro
dopasowuje go do socketu (proto/porty z cytatu) i ustawia `sk_err`; connected-UDP `recv()`
zwraca `-ECONNREFUSED`, TCP w SYN_SENT dostaje `-EHOSTUNREACH`/`-ECONNREFUSED`, raw sockety
widzą komunikat (już działa). Bez tego traceroute (FAZA I) nie ma jak działać.

**Files:**
- Modify: `net/Icmp.cpp` (parsowanie cytatu w `icmpRx` dla typów 3/11/12 → `icmpDeliverError`),
  `net/Udp.{h,cpp}` (`udpIcmpError(quote)` — demux do socketu po (lport,raddr,rport), tylko
  connected, jak Linux bez IP_RECVERR), `net/Tcp.{h,cpp}` (`tcpIcmpError` — w SYN_SENT ubij
  połączenie z odpowiednim errno; w ESTABLISHED ignoruj soft errors jak Linux), `net/Socket.h`
  (mapowanie code→errno: port-unreach→ECONNREFUSED, host-unreach→EHOSTUNREACH,
  net-unreach→ENETUNREACH, frag-needed→EMSGSIZE)
- Test: `tests/test_icmp_err.cpp`

- [x] **C1.** Test (failing): connected-UDP wysyła datagram → wstrzyknij ICMP port-unreachable
  cytujący ten datagram → `socketRecv` zwraca −ECONNREFUSED (raz — read-and-clear przez
  soError), kolejny recv blokuje normalnie. Drugi case: NIE-connected socket NIE dostaje błędu
  (semantyka Linuksa bez IP_RECVERR). Trzeci: TCP SYN_SENT + ICMP host-unreachable →
  `connect` kończy się −EHOSTUNREACH, stan CLOSED. Czwarty: cytat za krótki (hardening) — drop.
- [x] **C2.** Uruchomić: `make test` — nowe case'y FAIL.
- [x] **C3.** Implementacja: w `icmpRx` dla typów 3/11/12 zwaliduj cytat (IHL, długość ≥
  IP+8B), wyciągnij (proto, src, dst, sport, dport) z cytatu i wywołaj handler transportu.
  Budzenie czytelnika przez istniejący wake hook.
- [x] **C4.** `make test` PASS, ≥90% nowych linii; `check-arch` czysty.
- [x] **C5.** QEMU: `pingtest`-owy mini-probe UDP na zamknięty port 10.0.2.2 → konsola pokazuje
  ECONNREFUSED; pcap pokazuje przychodzący ICMP port-unreachable (pole-po-polu jak Linux,
  maska wg planu bazowego §2).
- [x] **C6.** Commit: `net: deliver inbound ICMP errors to matching sockets (Linux semantics)`.

### FAZA D — TCP: opcje i timery, których brakuje do „prawdziwego" Linuksa na drucie

Prowizorka #7. Linuksowy SYN to `[mss 1460, sackOK, TS val ecr, nop, wscale 7]` — nasz ma samo
mss. Do tego brak delayed ACK (Linux: do 40 ms / co drugi segment), persist timer (zero-window
probe — bez niego sesja z wyzerowanym oknem WISI na zawsze) i keepalive. To są rzeczy widoczne
bajtowo na drucie i behawioralnie w długich sesjach.

**Files:**
- Modify: `net/Tcp.{h,cpp}`
- Test: `tests/test_tcp.cpp` (rozszerzenie) + `tests/test_tcp_opts.cpp`

> **Odkryte w FAZIE B (2026-06-12):** `tcpTick` (net/Tcp.cpp:402-413) retransmituje przy RTO
> TYLKO dane (`chunk>0`) albo FIN — **NIE retransmituje SYN-a w `SYN_SENT`**. Połączenie zależy
> więc od pojedynczego SYN-a; gdy pierwszy SYN-ACK jest wolny (slirp potrafi zwlec ~11 s na
> pierwszym `connect()` do AWS) lub zgubiony, sesja wisi. To pre-istniejący bug, nie regresja —
> naprawić w tej fazie jako **D0** (najpierw, bo dotyka każdego z poniższych).

Zakres (wszystko, bez skrótów — kolejno, każde z własnym cyklem test-fail-impl-pass-commit):
- [x] **D0. SYN retransmit (RTO w SYN_SENT/SYN_RCVD):** gdy RTO odpali a `snd_una != snd_nxt` i
  nie ma danych/FIN, retransmituj SYN (SYN_SENT) lub SYN-ACK (SYN_RCVD), z backoffem i limitem
  prób (~5, potem `-ETIMEDOUT`/`-ECONNREFUSED` na sockecie). Test: brak SYN-ACK → 2. SYN po RTO,
  trzeci po 2×RTO; po limicie connect kończy się błędem.
- [x] **D1. Window scaling (RFC 7323 §2):** wysyłaj `wscale` w SYN/SYN-ACK (nasz shift: 2 —
  RCVBUF 8 KiB nie potrzebuje więcej, ale opcja MUSI być negocjowana jak w Linuksie), honoruj
  shift peera przy interpretacji jego okna. Test: handshake z wscale=7 od peera → wysyłka
  respektuje przeskalowane okno; SYN bez wscale → opcja wyłączona obustronnie.
- [x] **D2. Timestamps (RFC 7323 §3) + RTTM:** TS w każdym segmencie po negocjacji, echo TSecr,
  RTT z TS zamiast timera Karna tam, gdzie dostępne; PAWS na odbiorze (odrzuć segment z
  ts < ts_recent dla okna). Test: wektory ts_recent/PAWS + retransmisja mierzy RTT z TS.
- [x] **D3. SACK (RFC 2018):** wysyłaj `sackOK` w SYN; GENERUJ bloki SACK przy dziurach w
  odbiorze (mamy bufor OOO — bloki z niego); na nadawcy parsuj SACK i nie retransmituj
  zSACKowanych segmentów przy fast-retransmit. Test: zguba środkowego segmentu → nasz ACK
  niesie poprawny blok SACK (pole-po-polu); nadawca z SACK retransmituje TYLKO dziurę.
- [x] **D4. Delayed ACK:** ACK opóźniony do 40 ms lub natychmiast przy drugim pełnym MSS /
  pustym oknie / FIN (heurystyka Linuksa). Sterowany z `tcpTick` (50 ms tick wystarcza: jeden
  tick opóźnienia). Test: pojedynczy segment → ACK dopiero po ticku; dwa segmenty → ACK od razu.
- [x] **D5. Persist timer (zero-window probe):** gdy okno peera = 0 i mamy dane — probe 1 B
  z backoffem (5 s → max 60 s), aż okno się otworzy. Test: peer ogłasza 0 → probe na drucie →
  okno otwarte → wznowienie. Bez persist sesja wisiałaby wiecznie (to bug, nie feature-gap).
- [x] **D6. Keepalive:** `SO_KEEPALIVE` + `TCP_KEEPIDLE/INTVL/CNT` (defaulty Linuksa
  7200 s/75 s/9 — w testach skracane opcjami); sonda = ACK z seq−1; po CNT bez odpowiedzi →
  ETIMEDOUT na sockecie. Test: symulowany czas przez `tcpTick`.
- [x] **D7.** Bramka drutu: w QEMU sesja do 1.1.1.1:80 — nasz SYN niesie
  `[mss,sackOK,TS,nop,wscale]` w TEJ SAMEJ kolejności i formacie co Linux i686 (tcpdump,
  porównanie pole-po-polu; TS val w masce); zero faultów; cała suita TCP zielona.
- [x] **D8.** Commit per podpunkt: `tcp: window scaling`, `tcp: timestamps+PAWS`, `tcp: SACK`,
  `tcp: delayed ACK`, `tcp: persist timer`, `tcp: keepalive`.

### FAZA E — resolver i /etc: czytać NAPRAWDĘ to, co leży na dysku

Prowizorki #3 i #8. Pliki `/etc/services` i `/etc/protocols` są shipowane do obrazu i
IGNOROWANE — kod czyta wbudowane tabele. To dokładnie „podrutowane". Do tego resolver bez
TCP-fallbacku, search domains i PTR.

**Files:**
- Modify: `user/libc-glue/resolv.c`
- Test: `tests/` — moduł jest userlandowy; logika parserów wydzielona do funkcji czystych
  (wejście: bufor pliku / pakiet DNS) i testowana hostowo w `tests/test_resolv.cpp` przez
  kompilację `resolv.c` z `-DRESOLV_HOST_TEST` (bez syscalli; pattern jak NxeLoader)

- [x] **E1. `/etc/services` + `/etc/protocols`:** `getservbyname/byport`, `getprotobyname/
  bynumber` parsują pliki (format Linuksa: nazwa, port/proto, aliasy, komentarze `#`);
  wbudowane tabele zostają WYŁĄCZNIE jako fallback, gdy pliku nie ma (i to jest
  udokumentowane w kodzie). Test: wektor pliku z aliasami/komentarzami/tabami.
- [x] **E2. TCP/53 fallback:** odpowiedź z bitem TC → ponów zapytanie po TCP (2-bajtowy
  prefiks długości, RFC 1035 §4.2.2). Test: parser ścieżki TC + ramkowanie TCP.
- [x] **E3. `search`/`domain` + `ndots` z resolv.conf:** nazwa bez kropki (lub < ndots) →
  próby z sufiksami search listy, potem literal; nazwa z kropką → najpierw literal. Dokładny
  algorytm glibc/musl. Test: macierz nazwa×search×ndots.
- [x] **E4. Wiele nameserverów + retry/timeout:** wszystkie wpisy `nameserver` (max 3, jak
  MAXNS), timeout 5 s, 2 próby, rotacja przy braku odpowiedzi (`options timeout:/attempts:`
  honorowane). Test: symulacja braku odpowiedzi pierwszego ns.
- [x] **E5. Reverse DNS (PTR):** `gethostbyaddr`/`getnameinfo` budują `d.c.b.a.in-addr.arpa`,
  pytają o PTR, parsują (kompresja już jest); brak odpowiedzi → numeryczne (jak teraz, ale
  jako FALLBACK, nie jedyna ścieżka). Usuwa komentarz „does no reverse DNS" z `resolv.c:207`.
  Test: wektor odpowiedzi PTR z kompresją.
- [x] **E6.** QEMU: `ping -a`/`getnameinfo` przez slirp DNS — pcap pokazuje zapytanie PTR
  (pole-po-polu jak `dig -x`); `getservbyname("http","tcp")` czyta plik z dysku (usunąć wpis
  testowo → fallback działa).
- [x] **E7.** Commit per podpunkt (`resolv: read /etc/services|protocols`, `resolv: TCP
  fallback`, `resolv: search+ndots`, `resolv: multi-ns retry`, `resolv: PTR`).

### FAZA F — AF_PACKET + prawdziwy DHCP (domknięcie FAZY 10 planu bazowego)

Prowizorki #1 i #9 (część packet). Plan bazowy obiecał udhcpc-bez-łatek przez AF_PACKET i
odroczył; static-fallback działa, ale konfiguracja sieci wpisana w kernel to definicja
„podrutowane". Robimy dokładnie to, co plan bazowy specyfikował.

**Files:**
- Create: `net/Packet.{h,cpp}`, `tests/test_packet.cpp`
- Modify: `net/Socket.cpp` (AF_PACKET w socketCreate), `net/Ether.cpp` (tap RX dla packet
  socketów PRZED demuxem L3 + tap TX), `kernel/Syscall.cpp` (`sockaddr_ll` marshalling,
  `SIOCGIFINDEX`), `kernel/SyscallDispatch.cpp`, `kernel/NetCore.cpp` (bring-up: udhcpc
  zamiast statyki, statyka = fallback po timeout), Makefile (`make udhcpc`, instalacja
  `/nanos/bin/udhcpc.nxe` + skrypt `/nanos/config/udhcpc.script`), `user/libc-glue/include/
  netpacket/packet.h`, `net/if.h` (dopełnić), SDK manifest `udhcpc` (busybox, allyesconfig-
  minimal: tylko udhcpc)
- Test: `tests/test_packet.cpp`

- [x] **F1. AF_PACKET (MI core):** `SOCK_DGRAM` (cooked: kernel zdejmuje/dokłada nagłówek
  Ethernet, adresacja przez `sockaddr_ll`) i `SOCK_RAW` (pełna ramka), filtr po `sll_protocol`
  (ETH_P_IP/ETH_P_ALL, network order), `bind` po `sll_ifindex`. RX-tap w `ethRx` (kopia do
  pasujących packet socketów zanim ramka pójdzie do ARP/IP), TX przez `dev->tx` z pominięciem
  routingu. **Dostarczanie działa też, gdy interfejs nie ma adresu IP** — to jest cały sens
  (DHCP DISCOVER z 0.0.0.0). Testy: bind+filter, cooked TX (ramka na drucie pole-po-polu),
  RX ramki broadcast na nieskonfigurowanym dev, ETH_P_ALL widzi i ARP i IP.
- [x] **F2. Syscall ABI:** `sockaddr_ll` (układ Linux i686: family/protocol/ifindex/hatype/
  pkttype/halen/addr[8]) w sendto/recvfrom/bind; `SIOCGIFINDEX`. Test ABI: offsetof/sizeof.
- [x] **F3. Port busybox udhcpc** przez nanos-port (manifest jak inne porty; busybox skonfiguro-
  wany WYŁĄCZNIE z apletem udhcpc) — **bez łatek źródeł**; cache'ujemy detekcje gnulib-style
  w config. Skrypt akcji (`udhcpc.script`, shell uruchamiany przez busybox z env: ip/mask/
  router/dns) → woła `ifconfig`-owe ioctl-e przez mały helper LUB zapisuje pliki + sygnał;
  wybrać wariant Linuksa: skrypt ustawia adres ioctl-ami (SIOCSIFADDR/SIOCSIFNETMASK),
  trasę (SIOCADDRT — dociągnięty w F4) i przepisuje `/etc/resolv.conf`.
- [x] **F4. `SIOCADDRT` naprawdę** (prowizorka #5): parsować pełny `struct rtentry` Linuksa
  (rt_dst/rt_gateway/rt_genmask jako sockaddr_in, RTF_GATEWAY/RTF_UP) zamiast „minimally";
  + `SIOCDELRT`. Test: rtentry z gateway → wpis w tablicy tras zgodny z routeLookup.
- [x] **F5. Bring-up:** init odpala `udhcpc -i eth0 -q` przy boocie (przez istniejący
  mechanizm init→shell); kernelowy `netBringUp()` zostaje TYLKO jako fallback, gdy
  `/nanos/bin/udhcpc.nxe` nie istnieje lub nie skonfiguruje w 10 s (log na konsolę, która
  ścieżka zadziałała — żadnego cichego fallbacku).
- [x] **F6.** Bramka drutu: pcap pełnej wymiany DISCOVER→OFFER→REQUEST→ACK pole-po-polu jak
  `udhcpc` na Linuksie (maska: xid, secs); po boocie eth0 = 10.0.2.15 Z DHCP (konsola), trasa
  default, resolv.conf przepisany przez skrypt; `ping wp.pl` + `wget` działają jak dotąd.
- [x] **F7.** Commity: `net: AF_PACKET sockets`, `syscall: sockaddr_ll + SIOCGIFINDEX +
  real rtentry SIOCADDRT/DELRT`, `ports: busybox udhcpc`, `boot: DHCP bring-up (static as
  loud fallback)`.

### FAZA G — AF_UNIX + socketpair (lokalna komunikacja jak w Linuksie)

Prowizorka #9 (część unix). Każdy prawdziwy Unix ma AF_UNIX; serwisy (syslog), narzędzia i
gnulib-owe testy go oczekują. Mamy już wszystkie klocki: refcountowane obiekty fd (Pipe),
WaitQueue, VFS z RamFs.

**Files:**
- Create: `net/Unix.{h,cpp}`, `tests/test_unix.cpp`
- Modify: `net/Socket.cpp` (domena AF_UNIX → dispatch do Unix.cpp), `kernel/Syscall.cpp`
  (`sockaddr_un` marshalling; bind tworzy plik typu socket w VFS — wpis S_IFSOCK w RamFs/ext
  readonly → tylko /tmp i ramfs), `kernel/SyscallDispatch.cpp` (socketpair → realny),
  `net/NetProc.cpp` (+`/proc/net/unix`), `fs/SynthFs.cpp` (rejestracja generatora)
- Test: `tests/test_unix.cpp`

- [x] **G1. SOCK_STREAM:** refcountowany `UnixChannel` (dwa bajtowe ringi per kierunek + oba
  endpointy), `bind` na ścieżkę (EADDRINUSE gdy zajęta — autorytet: skan g_socks), `listen/
  accept/connect` przez WaitQueue, `ECONNREFUSED` gdy nikt nie słucha. Testy: pełny cykl
  client/server, EOF przy close, MSG_PEEK. **REFINEMENT ZROBIONY:** widoczny węzeł `S_IFSOCK`
  w VFS — `bind` woła `vfs->mknod` (RamFs S_IFSOCK), `ls -l /tmp` pokazuje `srwxrwxrwx
  unixtest.sock`, stary plik blokuje rebind do `unlink` (pełna semantyka Linuksa); graceful
  fallback do rejestru MI, gdy ścieżka nie jest na zapisywalnym FS (host-testy).
- [x] **G2. SOCK_DGRAM:** datagramy z zachowaniem granic (RXQ ring per socket + socketDeliver),
  `sendto` po ścieżce + `connect` ustawia domyślny cel. Test: granice komunikatów, ECONNREFUSED
  na nieistniejącą ścieżkę.
- [x] **G3. `socketpair(AF_UNIX, SOCK_STREAM)`:** zamienia ENOSYS z `SyscallDispatch.cpp:135`
  na prawdziwą parę połączonych socketów. Test: pisz-czytaj w obu kierunkach + QEMU smoke.
- [x] **G4. `/proc/net/unix`** w formacie Linuksa (Num RefCount Protocol Flags Type St Inode
  Path). Test renderera w test_netproc-stylu.
- [x] **G5.** `make test` 91.3% agregat (Unix.cpp 96%, NetProc.cpp 94%); QEMU: `unixtest` demo
  przez bash = ALL PASS, zero faultów. Commity: `net: AF_UNIX core`, `syscall: AF_UNIX ABI +
  socketpair`, `net: /proc/net/unix`, `test: unixtest`.

### FAZA H — SERWISY: inetd + telnetd + httpd (system, który można ODWIEDZIĆ)

Prowizorka #2 — odpowiedź na pytanie „czy tam nie ma serwisów?". Nie ma. Po tej fazie:
z macOS-owego hosta `telnet localhost 2323` loguje do basha na NanOS, a `curl
http://localhost:5555/` dostaje stronę z NanOS-owego httpd. To jest dowód „prawdziwości"
serwerowej strony stosu (listen/accept/wiele połączeń), tak jak ping/wget były dowodem
klienckiej.

**Files:**
- Modify: `user/libc-glue/posixstubs.c` → przenieść openpty/forkpty/login_tty do nowego
  `user/libc-glue/pty.c` z REALNĄ implementacją (kernel ma PTY: /dev/ptmx-odpowiednik z
  nterm — sprawdzić dokładny interfejs w `kernel/Termios.h`/`LineDiscipline.h` i nterm),
  Makefile (`make inetd`, `make telnetd`, `make httpd`, instalacja + hostfwd
  `tcp::2323-:23`), `disk` template: `/nanos/config/etc/inetd.conf`, init/rc: start inetd
- Create: manifesty portów w SDK (inetutils inetd+telnetd+telnet — JEDEN build inetutils,
  jak ping; darkhttpd — pojedynczy plik C, zero zależności), `scripts/services-qemu.sh`
  (boot headless + hostfwd + test z hosta + pcap)

- [x] **H1. Realne `openpty`/`forkpty`/`login_tty`** (`user/libc-glue/pty.c`): nad parą PTY jądra
  (/dev/ptmx + /dev/pts0). openpty otwiera obie + zwraca nazwę slave'a; forkpty fork + login_tty
  w dziecku; login_tty = setsid + dup2(slave→0/1/2). Kompiluje+linkuje do libc.ndl. **UWAGA:**
  jądro ma JEDNĄ parę PTY → jedna sesja naraz (wystarcza na jeden login telnet = akceptacja H);
  wielosesyjność wymaga dynamicznej alokacji PTY w jądrze (osobna zmiana, poza H).
- [x] **H2. Port inetd (inetutils):** built-iny echo/discard/daytime/chargen + uruchamianie
  usług z `inetd.conf` (nowait/stream: accept→fork→dup2 socket na 0/1/2→exec — wszystkie
  te syscalle JUŻ działają). `inetd.conf` shipowany (echo/discard/daytime/chargen tcp+udp).
  **ZROBIONE:** `make inetd` (services manifest, servers on), zainstalowany w /nanos/bin; dodano
  syscalle pause/sigsuspend + wrappery wait/execv (libc-glue). Z hosta przez hostfwd: daytime
  (5013→13) zwraca datę, echo (5007→7) odbija bajty, zero faultów, pełne handshake'y w pcap
  (scripts/services-qemu.sh). Pierwszy realny konsument listen/accept; tcpsrv.c + tcpsrv-diag.sh
  izolują i dowodzą ścieżkę passive-open. UWAGA: krótkie opcje argp psute przez getopt picolibc —
  inetd startuje z długimi opcjami + pozycyjną ścieżką configu (`/disks/main/nanos/config/etc/`).
- [x] **H3. Port telnetd (inetutils):** przez inetd; negocjacja opcji telnet, pty przez H1,
  exec login-shella. **ZROBIONE:** telnetd z tego samego services-manifestu (--enable-telnetd,
  mknx w hooks/post_build.sh); dopełniono nagłówki sysroot (telnet.h LINEMODE+SLC+ENVIRON,
  syslog.h facilities, ioctl.h FIONBIO+TIOCPKT) bez łatek źródeł. Nowy `nanologin` (single-user
  root: env + getpwuid->pw_shell) uruchamiany przez `telnetd --exec-login=`. Wymagało dwóch
  napraw jądra/libc: **TIOCPKT packet-mode w pty** (telnetd odrzuca pierwszy bajt każdego odczytu
  mastera jako preambułę) i **login_tty ustawia controlling-tty + foreground pgrp** (`tcsetpgrp`),
  bez czego bash SIGTTIN-stopował się po cichu. Z macOS `telnet localhost 2323` → `bash-5.2#`,
  `ls /` działa, `exit` czysto; DRUGA sesja reużywa jedną parę pty (brak wycieku); zero faultów,
  pełna negocjacja IAC na drucie (scripts/telnet-qemu.sh).
- [x] **H4. Port darkhttpd:** single-file HTTP/1.1, serwuje `/disks/main/apps/www` (index.html
  w obrazie). **ZROBIONE:** `make httpd` (build=make, -DNO_IPV6, bez łatek); dorobiono libc
  pread/pwrite + stuby chroot/getrusage. Wymagało DWÓCH realnych napraw TCP: **honorować
  TCP_NODELAY** (był no-op-kłamstwo) i **tcpClose flush bufora nadawczego przed FIN** (ciało HTTP
  zapisane tuż przed close ginęło na drucie — nagłówek docierał, body nie). Z hosta `curl`
  (5555→80) → 200 + pełne body; **4 jednoczesne GET-y = 4/4** 200+body (TCB_N=16 z FAZY B); zero
  faultów; regression test w test_tcp.cpp (Nagle-held body flushowane przed FIN). scripts/httpd-qemu.sh.
- [x] **H5. rc/init:** start inetd + httpd przy boocie. **ZROBIONE:** init.c `start_services()` po
  DHCP, przed exec shella (fork+exec+reap; serwisy demonizują się, grandchild reparentuje do init);
  brak binarki = pominięcie. Pełna akceptacja BEZ wpisywania na konsoli (scripts/boot-services-qemu.sh):
  curl :5555→200+strona, daytime :5013, echo :5007, telnet :2323→bash — wszystkie zielone, zero
  faultów. Soak/leak: pętla 40 GET wyłapała wyczerpanie TCB (active-close → TIME_WAIT × TCB_N=16);
  naprawione **recyklingiem najstarszego TIME_WAIT pod presją** (Linux tcp_tw_reuse) → 40/40,
  serwer żyje (scripts/svc-loop-qemu.sh).
- [x] **H6.** Commity (bez wzmianki o AI): pause/sigsuspend/wait/execv; inetutils inetd; pty
  TIOCPKT+login_tty; inetutils telnetd; TCP_NODELAY+close-flush; pread/pwrite+stuby; darkhttpd;
  boot start-services; TIME_WAIT recycle.

### FAZA I — narzędzia diagnostyczne (dowód, że ioctl-e i ICMP-errors są prawdziwe)

**Files:**
- Modify: Makefile (`make nettools` — z TEGO SAMEGO builda inetutils co ping/inetd:
  ifconfig, traceroute, telnet klient, hostname), instalacja do `/nanos/bin`

- [x] **I1. `ifconfig`** (inetutils): **ZROBIONE.** Enumeruje przez `if_nameindex` (dorobione
  if_nametoindex/indextoname/nameindex/freenameindex w libc-glue ifname.c nad SIOCGIFINDEX — NIE
  trzeba było SIOCGIFCONF). QEMU: `ifconfig` pokazuje eth0 (10.0.2.15 z DHCP) + lo z
  addr/netmask/broadcast/flags/mtu.
- [x] **I2. `traceroute`** (inetutils): **ZROBIONE.** Wymagało **setsockopt(IPPROTO_IP, IP_TTL)**
  (dorobione: per-socket ttl + plumbing przez ipOutput) — to ćwiczy też FAZĘ C (odbiór ICMP
  time-exceeded). QEMU: `traceroute 10.0.2.2` → `1  10.0.2.2  1.000ms`, hop 2 `*` (slirp NAT zwija
  ścieżkę — poprawne); pcap: UDP TTL 1..n + ICMP time-exceeded.
- [x] **I3. `telnet` (klient):** **ZROBIONE.** Wymagało **trasy loopback 127/8→lo** (brakowało —
  self-connect szedł domyślną trasą i był odrzucany) + execl/TCFLSH w libc. QEMU: `telnet 127.0.0.1
  23` → Connected → baner telnetd → `bash-5.2# ls /` → dev/disks/proc/tmp; serwis↔klient 100% na
  NanOS, zero hosta, zero faultów.
- [x] **I4.** Commity: `net: per-socket IP_TTL + loopback route`, `libc+ports: inetutils nettools`.
  UWAGA (jak H2): krótkie opcje argp psute przez getopt picolibc nawet z wymuszonym REPLACE_GETOPT
  — klienty sterowane długimi opcjami; właściwy fix getopt w libc to osobny tracked item.

### FAZA J — sweep końcowy: dokumentacja, /proc, hardening nowych powierzchni

- [x] **J1.** `tests/test_net_hardening.cpp` — **ZROBIONE:** złośliwe/obcięte błędy ICMP (cytat
  kłamiący o protokole/portach, cytat < IP+8) demux-and-drop bez mis-delivery/wycieku; SYN-flood
  (100 SYN) na listener nie wyczerpuje tablicy TCB ani nie wycieka bufów. 546 testów.
- [x] **J2.** `/proc/net/tcp` — **ZWERYFIKOWANE:** wszystkie 6 listenerów (echo/discard/daytime/
  chargen/telnet/http) pokazują stan `0A` (LISTEN) w formacie Linuksa.
- [x] **J3.** `docs/networking.md` — **PRZEPISANE:** §5 (DHCP+fallback), §9 (serwisy+narzędzia +
  quirk getopt), §10 (tabela różnic odzwierciedla rzeczywistość: AF_UNIX/PACKET realne, DHCP,
  TIME-WAIT recycle, trasa 127/8), §13 (out-of-scope przycięte).
- [x] **J4.** **ZROBIONE:** `make test` 546 testów / 91.1% agregat, `check-arch` czysty; pełny
  scenariusz QEMU (boot→DHCP→serwisy z hosta→traceroute→telnet→/proc/net) zero faultów; obraz
  **e2fsck-clean** (dodano `e2fsck -fy` na końcu `_image`).
- [x] **J5.** Commit + progress-log `2026-06-12-net-dociagniecia-progress.md` (tabela A–J, wszystkie
  DONE + kryteria akceptacji spełnione).

---

## Poza zakresem (świadomie, to NIE są prowizorki tylko przyszłe projekty)

- **TLS/HTTPS** — wymaga portu OpenSSL/GnuTLS (osobny, duży plan).
- **IPv6** — osobny plan (dotyka każdej warstwy).
- **sshd** — wymaga TLS-klasy krypto; telnetd jest uczciwym odpowiednikiem na dziś.
- **netfilter/iptables, forwarding, multiple routing tables** — NanOS jest hostem, nie routerem.
- **Offload sprzętowy (csum/TSO), NAPI polling** — przy 1 NIC w QEMU brak uzasadnienia.
- **/proc/sys/net (zapisywalne sysctl-e)** — do rozważenia przy następnym podejściu; dziś
  read-only /proc jest uczciwie udokumentowane.

## Kryteria akceptacji całości

1. **Z hosta macOS:** `telnet localhost 2323` → login do basha na NanOS; `curl
   http://localhost:5555/` → 200 z darkhttpd. (Serwisy istnieją i działają.)
2. **DHCP, nie statyka:** boot konfiguruje eth0 przez prawdziwego busybox-udhcpc (pcap
   DISCOVER→ACK pole-po-polu jak Linux); statyka tylko jako głośny fallback.
3. **Nasz SYN = linuksowy SYN:** `[mss,sackOK,TS,nop,wscale]`; delayed ACK, persist i
   keepalive obecne behawioralnie (testy + pcap).
4. **`/etc` jest prawdą:** services/protocols/resolv.conf(search,ndots,multi-ns)/PTR czytane
   z plików; traceroute działa (ICMP errors docierają do socketów).
5. **AF_UNIX + socketpair + AF_PACKET** prawdziwe; `/proc/net/unix` w formacie Linuksa.
6. Wszystkie porty **bez łatek źródeł** (tylko manifesty/config.cache, jak ping/wget);
   `make test` ≥90%, `check-arch` czysty, QEMU bez `v=08/0d/0e`; commity bez wzmianki o AI.
