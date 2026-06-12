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

- [ ] **A1.** W tabeli §10 wiersz `AF_UNIX / AF_PACKET`: zamienić `AF_UNIX declared (ENOSYS),
  AF_PACKET partial` na `both: constants in headers only — socket() returns EAFNOSUPPORT
  (socketpair: ENOSYS)`. Stan faktyczny: `net/Socket.cpp:35` (EAFNOSUPPORT dla ≠AF_INET),
  `kernel/SyscallDispatch.cpp:135` (socketpair −38).
- [ ] **A2.** W §12 zamienić „byte-compared" na „field-compared (with an explicit mask of
  legitimately random fields — IP ID, ISN, ephemeral ports, DNS ID; see the plan §2 gate)".
- [ ] **A3.** W §5 i §13 dopisać odnośnik do tego planu jako „follow-up in progress".
- [ ] **A4.** Commit: `docs: correct networking.md (AF_UNIX/AF_PACKET reality, field-compare wording)`.

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

- [ ] **B1.** Zinwentaryzować WSZYSTKIE wystąpienia stałej 0x400000/0x500000 (grep po kernel/,
  user/, arch/, docs/): load base, stack top, granica user-window w AddressSpace, walidacje
  wskaźników user w Syscall. Wynik = checklista zmian (jedna stała w jednym miejscu per moduł).
- [ ] **B2.** Podnieść bazę okna usera do **0x800000** (8 MiB): kernel ma wtedy ~5 MiB
  headroomu (dziś 3.03 MB kończy się @0x3e2f98). Stack top usera analogicznie (+0x100000 nad
  obrazem jak dziś). Wszystkie programy .nxe i .ndl są linkowane naszym `nx.ld` — wymaga
  PRZEBUDOWY całego userlandu i portów (`make _userland`, `make bash/grep/vim/ping/wget`),
  ale ZERO zmian źródeł.
- [ ] **B3.** Odbudować pule: `POOL_N=128`, `TCB_N=16`, `SNDBUF/RCVBUF=8192`. Backlog zostaje
  64 (nadal < pool — punkt dropu się nie zmienia; zaktualizować komentarz w `NetBuf.cpp:10`).
- [ ] **B4.** `make test` zielone; QEMU: boot + bash + `ping wp.pl` + `wget` (pełna regresja
  starych portów na nowej bazie), `readelf`/mapfile potwierdza koniec kernela < 0x800000 z
  zapasem; zero `v=08/0d/0e`.
- [ ] **B5.** Zaktualizować `docs/networking.md` §11 (nowe wartości) + CLAUDE.md (mapa pamięci).
- [ ] **B6.** Commit: `mm+net: raise user window to 0x800000, restore right-sized net pools`.

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

- [ ] **C1.** Test (failing): connected-UDP wysyła datagram → wstrzyknij ICMP port-unreachable
  cytujący ten datagram → `socketRecv` zwraca −ECONNREFUSED (raz — read-and-clear przez
  soError), kolejny recv blokuje normalnie. Drugi case: NIE-connected socket NIE dostaje błędu
  (semantyka Linuksa bez IP_RECVERR). Trzeci: TCP SYN_SENT + ICMP host-unreachable →
  `connect` kończy się −EHOSTUNREACH, stan CLOSED. Czwarty: cytat za krótki (hardening) — drop.
- [ ] **C2.** Uruchomić: `make test` — nowe case'y FAIL.
- [ ] **C3.** Implementacja: w `icmpRx` dla typów 3/11/12 zwaliduj cytat (IHL, długość ≥
  IP+8B), wyciągnij (proto, src, dst, sport, dport) z cytatu i wywołaj handler transportu.
  Budzenie czytelnika przez istniejący wake hook.
- [ ] **C4.** `make test` PASS, ≥90% nowych linii; `check-arch` czysty.
- [ ] **C5.** QEMU: `pingtest`-owy mini-probe UDP na zamknięty port 10.0.2.2 → konsola pokazuje
  ECONNREFUSED; pcap pokazuje przychodzący ICMP port-unreachable (pole-po-polu jak Linux,
  maska wg planu bazowego §2).
- [ ] **C6.** Commit: `net: deliver inbound ICMP errors to matching sockets (Linux semantics)`.

### FAZA D — TCP: opcje i timery, których brakuje do „prawdziwego" Linuksa na drucie

Prowizorka #7. Linuksowy SYN to `[mss 1460, sackOK, TS val ecr, nop, wscale 7]` — nasz ma samo
mss. Do tego brak delayed ACK (Linux: do 40 ms / co drugi segment), persist timer (zero-window
probe — bez niego sesja z wyzerowanym oknem WISI na zawsze) i keepalive. To są rzeczy widoczne
bajtowo na drucie i behawioralnie w długich sesjach.

**Files:**
- Modify: `net/Tcp.{h,cpp}`
- Test: `tests/test_tcp.cpp` (rozszerzenie) + `tests/test_tcp_opts.cpp`

Zakres (wszystko, bez skrótów — kolejno, każde z własnym cyklem test-fail-impl-pass-commit):
- [ ] **D1. Window scaling (RFC 7323 §2):** wysyłaj `wscale` w SYN/SYN-ACK (nasz shift: 2 —
  RCVBUF 8 KiB nie potrzebuje więcej, ale opcja MUSI być negocjowana jak w Linuksie), honoruj
  shift peera przy interpretacji jego okna. Test: handshake z wscale=7 od peera → wysyłka
  respektuje przeskalowane okno; SYN bez wscale → opcja wyłączona obustronnie.
- [ ] **D2. Timestamps (RFC 7323 §3) + RTTM:** TS w każdym segmencie po negocjacji, echo TSecr,
  RTT z TS zamiast timera Karna tam, gdzie dostępne; PAWS na odbiorze (odrzuć segment z
  ts < ts_recent dla okna). Test: wektory ts_recent/PAWS + retransmisja mierzy RTT z TS.
- [ ] **D3. SACK (RFC 2018):** wysyłaj `sackOK` w SYN; GENERUJ bloki SACK przy dziurach w
  odbiorze (mamy bufor OOO — bloki z niego); na nadawcy parsuj SACK i nie retransmituj
  zSACKowanych segmentów przy fast-retransmit. Test: zguba środkowego segmentu → nasz ACK
  niesie poprawny blok SACK (pole-po-polu); nadawca z SACK retransmituje TYLKO dziurę.
- [ ] **D4. Delayed ACK:** ACK opóźniony do 40 ms lub natychmiast przy drugim pełnym MSS /
  pustym oknie / FIN (heurystyka Linuksa). Sterowany z `tcpTick` (50 ms tick wystarcza: jeden
  tick opóźnienia). Test: pojedynczy segment → ACK dopiero po ticku; dwa segmenty → ACK od razu.
- [ ] **D5. Persist timer (zero-window probe):** gdy okno peera = 0 i mamy dane — probe 1 B
  z backoffem (5 s → max 60 s), aż okno się otworzy. Test: peer ogłasza 0 → probe na drucie →
  okno otwarte → wznowienie. Bez persist sesja wisiałaby wiecznie (to bug, nie feature-gap).
- [ ] **D6. Keepalive:** `SO_KEEPALIVE` + `TCP_KEEPIDLE/INTVL/CNT` (defaulty Linuksa
  7200 s/75 s/9 — w testach skracane opcjami); sonda = ACK z seq−1; po CNT bez odpowiedzi →
  ETIMEDOUT na sockecie. Test: symulowany czas przez `tcpTick`.
- [ ] **D7.** Bramka drutu: w QEMU sesja do 1.1.1.1:80 — nasz SYN niesie
  `[mss,sackOK,TS,nop,wscale]` w TEJ SAMEJ kolejności i formacie co Linux i686 (tcpdump,
  porównanie pole-po-polu; TS val w masce); zero faultów; cała suita TCP zielona.
- [ ] **D8.** Commit per podpunkt: `tcp: window scaling`, `tcp: timestamps+PAWS`, `tcp: SACK`,
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

- [ ] **E1. `/etc/services` + `/etc/protocols`:** `getservbyname/byport`, `getprotobyname/
  bynumber` parsują pliki (format Linuksa: nazwa, port/proto, aliasy, komentarze `#`);
  wbudowane tabele zostają WYŁĄCZNIE jako fallback, gdy pliku nie ma (i to jest
  udokumentowane w kodzie). Test: wektor pliku z aliasami/komentarzami/tabami.
- [ ] **E2. TCP/53 fallback:** odpowiedź z bitem TC → ponów zapytanie po TCP (2-bajtowy
  prefiks długości, RFC 1035 §4.2.2). Test: parser ścieżki TC + ramkowanie TCP.
- [ ] **E3. `search`/`domain` + `ndots` z resolv.conf:** nazwa bez kropki (lub < ndots) →
  próby z sufiksami search listy, potem literal; nazwa z kropką → najpierw literal. Dokładny
  algorytm glibc/musl. Test: macierz nazwa×search×ndots.
- [ ] **E4. Wiele nameserverów + retry/timeout:** wszystkie wpisy `nameserver` (max 3, jak
  MAXNS), timeout 5 s, 2 próby, rotacja przy braku odpowiedzi (`options timeout:/attempts:`
  honorowane). Test: symulacja braku odpowiedzi pierwszego ns.
- [ ] **E5. Reverse DNS (PTR):** `gethostbyaddr`/`getnameinfo` budują `d.c.b.a.in-addr.arpa`,
  pytają o PTR, parsują (kompresja już jest); brak odpowiedzi → numeryczne (jak teraz, ale
  jako FALLBACK, nie jedyna ścieżka). Usuwa komentarz „does no reverse DNS" z `resolv.c:207`.
  Test: wektor odpowiedzi PTR z kompresją.
- [ ] **E6.** QEMU: `ping -a`/`getnameinfo` przez slirp DNS — pcap pokazuje zapytanie PTR
  (pole-po-polu jak `dig -x`); `getservbyname("http","tcp")` czyta plik z dysku (usunąć wpis
  testowo → fallback działa).
- [ ] **E7.** Commit per podpunkt (`resolv: read /etc/services|protocols`, `resolv: TCP
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

- [ ] **F1. AF_PACKET (MI core):** `SOCK_DGRAM` (cooked: kernel zdejmuje/dokłada nagłówek
  Ethernet, adresacja przez `sockaddr_ll`) i `SOCK_RAW` (pełna ramka), filtr po `sll_protocol`
  (ETH_P_IP/ETH_P_ALL, network order), `bind` po `sll_ifindex`. RX-tap w `ethRx` (kopia do
  pasujących packet socketów zanim ramka pójdzie do ARP/IP), TX przez `dev->tx` z pominięciem
  routingu. **Dostarczanie działa też, gdy interfejs nie ma adresu IP** — to jest cały sens
  (DHCP DISCOVER z 0.0.0.0). Testy: bind+filter, cooked TX (ramka na drucie pole-po-polu),
  RX ramki broadcast na nieskonfigurowanym dev, ETH_P_ALL widzi i ARP i IP.
- [ ] **F2. Syscall ABI:** `sockaddr_ll` (układ Linux i686: family/protocol/ifindex/hatype/
  pkttype/halen/addr[8]) w sendto/recvfrom/bind; `SIOCGIFINDEX`. Test ABI: offsetof/sizeof.
- [ ] **F3. Port busybox udhcpc** przez nanos-port (manifest jak inne porty; busybox skonfiguro-
  wany WYŁĄCZNIE z apletem udhcpc) — **bez łatek źródeł**; cache'ujemy detekcje gnulib-style
  w config. Skrypt akcji (`udhcpc.script`, shell uruchamiany przez busybox z env: ip/mask/
  router/dns) → woła `ifconfig`-owe ioctl-e przez mały helper LUB zapisuje pliki + sygnał;
  wybrać wariant Linuksa: skrypt ustawia adres ioctl-ami (SIOCSIFADDR/SIOCSIFNETMASK),
  trasę (SIOCADDRT — dociągnięty w F4) i przepisuje `/etc/resolv.conf`.
- [ ] **F4. `SIOCADDRT` naprawdę** (prowizorka #5): parsować pełny `struct rtentry` Linuksa
  (rt_dst/rt_gateway/rt_genmask jako sockaddr_in, RTF_GATEWAY/RTF_UP) zamiast „minimally";
  + `SIOCDELRT`. Test: rtentry z gateway → wpis w tablicy tras zgodny z routeLookup.
- [ ] **F5. Bring-up:** init odpala `udhcpc -i eth0 -q` przy boocie (przez istniejący
  mechanizm init→shell); kernelowy `netBringUp()` zostaje TYLKO jako fallback, gdy
  `/nanos/bin/udhcpc.nxe` nie istnieje lub nie skonfiguruje w 10 s (log na konsolę, która
  ścieżka zadziałała — żadnego cichego fallbacku).
- [ ] **F6.** Bramka drutu: pcap pełnej wymiany DISCOVER→OFFER→REQUEST→ACK pole-po-polu jak
  `udhcpc` na Linuksie (maska: xid, secs); po boocie eth0 = 10.0.2.15 Z DHCP (konsola), trasa
  default, resolv.conf przepisany przez skrypt; `ping wp.pl` + `wget` działają jak dotąd.
- [ ] **F7.** Commity: `net: AF_PACKET sockets`, `syscall: sockaddr_ll + SIOCGIFINDEX +
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

- [ ] **G1. SOCK_STREAM:** para buforów w pamięci (jak dwukierunkowy Pipe), `bind` na ścieżkę
  (plik S_IFSOCK w VFS; EADDRINUSE gdy istnieje), `listen/accept/connect` przez WaitQueue,
  `ECONNREFUSED` gdy nikt nie słucha. Testy: pełny cykl client/server, EOF przy close,
  refcount przez fork (wzorzec socketRef już jest).
- [ ] **G2. SOCK_DGRAM:** datagramy z zachowaniem granic (RXQ ring per socket — reuse
  istniejącego), `sendto` po ścieżce. Test: granice komunikatów, ENOENT na złą ścieżkę.
- [ ] **G3. `socketpair(AF_UNIX, SOCK_STREAM)`:** zamienia ENOSYS z `SyscallDispatch.cpp:135`
  na prawdziwą parę połączonych socketów. Test: pisz-czytaj w obu kierunkach + fork.
- [ ] **G4. `/proc/net/unix`** w formacie Linuksa (Num RefCount Protocol Flags Type St Inode
  Path). Test renderera w test_netproc-stylu.
- [ ] **G5.** `make test` ≥90% nowych modułów; QEMU: program demo socketpair przez bash;
  zero faultów. Commit: `net: AF_UNIX (stream+dgram) + real socketpair + /proc/net/unix`.

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

- [ ] **H1. Realne `openpty`/`forkpty`/`login_tty`** (prowizorka #4): nad istniejącym PTY
  jądra (ten sam mechanizm, którego używa nterm). To jest klucz do telnetd i do przyszłych
  sshd/screen. Test: host-testowalna część (alokacja pary, ujście danych) + QEMU smoke
  (program forkpty→bash→echo).
- [ ] **H2. Port inetd (inetutils):** built-iny echo/discard/daytime/chargen + uruchamianie
  usług z `/etc/inetd.conf` (nowait/stream: accept→fork→dup2 socket na 0/1/2→exec — wszystkie
  te syscalle JUŻ działają). `inetd.conf` shipowany: echo, daytime, telnet→telnetd.
  Weryfikacja: z hosta `nc localhost <port>` na echo/daytime przez hostfwd.
- [ ] **H3. Port telnetd (inetutils):** przez inetd; negocjacja opcji telnet, pty przez H1,
  exec login-shella (`getpwuid`→`pw_shell` — mechanizm logowania już jest, używa go nterm/init).
  Weryfikacja: z macOS `telnet localhost 2323` → bash prompt na NanOS, `ls /` działa,
  `exit` zamyka sesję czysto (brak wycieku socketów/pty — sprawdzić licznikami).
- [ ] **H4. Port darkhttpd:** single-file, HTTP/1.1, serwuje katalog — wystawić
  `/disks/main/apps/www` (dorzucić index.html do obrazu). Weryfikacja: `curl
  http://localhost:5555/` z hosta zwraca 200 + treść; pcap pokazuje POPRAWNY serwerowy
  handshake (nasz SYN-ACK z opcjami z FAZY D), transfer i zamknięcie; test równoległości:
  4 jednoczesne curl-e (limit TCB_N=16 z FAZY B daje zapas).
- [ ] **H5. rc/init:** start inetd + httpd przy boocie (wpis w istniejącym mechanizmie
  init→shell; logi na konsolę). QEMU bez faultów przy wielogodzinnym idle z nasłuchującymi
  serwisami (test: boot + 10 min + 100 połączeń pętlą — liczniki netbufInUse wracają do bazy).
- [ ] **H6.** Commity: `libc: real openpty/forkpty over kernel pty`, `ports: inetutils inetd`,
  `ports: telnetd — remote bash login`, `ports: darkhttpd`, `boot: start services`.

### FAZA I — narzędzia diagnostyczne (dowód, że ioctl-e i ICMP-errors są prawdziwe)

**Files:**
- Modify: Makefile (`make nettools` — z TEGO SAMEGO builda inetutils co ping/inetd:
  ifconfig, traceroute, telnet klient, hostname), instalacja do `/nanos/bin`

- [ ] **I1. `ifconfig`** (inetutils): pokazuje eth0/lo z adresami z DHCP — ćwiczy
  SIOCGIFCONF/SIOCGIFADDR/FLAGS/HWADDR/MTU na żywym systemie. (SIOCGIFCONF dopisać, jeśli
  inetutils go używa — strace na Linuksie najpierw, jak w FAZIE 0 planu bazowego.)
- [ ] **I2. `traceroute`** (inetutils): UDP z rosnącym TTL + odbiór ICMP time-exceeded —
  to jest test końcowy FAZY C (bez niej traceroute milczy). Weryfikacja: `traceroute 10.0.2.2`
  pokazuje hop; pcap: serie UDP z TTL 1..n + ICMP time-exceeded (uwaga: slirp może skracać
  ścieżkę do 1 hopu — wynik „1 hop do gw" JEST poprawny w NAT).
- [ ] **I3. `telnet` (klient):** interaktywny test naszego własnego telnetd przez lo
  (`telnet 127.0.0.1 23`) — pętla serwis↔klient w 100% na NanOS, zero hosta.
- [ ] **I4.** Commit: `ports: inetutils nettools (ifconfig, traceroute, telnet)`.

### FAZA J — sweep końcowy: dokumentacja, /proc, hardening nowych powierzchni

- [ ] **J1.** `tests/test_net_hardening.cpp` — rozszerzyć o nowe powierzchnie: złośliwe ICMP
  errors (cytat kłamiący o protokole/portach, cytat obcięty), ramki do AF_PACKET na martwym
  ifindexie, śmieci w sockaddr_un (ścieżka bez NUL), telnet-negotiation fuzzing NIE dotyczy
  jądra (userland) — ale złośliwy klient inetd (SYN flood na listen backlog) nie może
  wyczerpać TCB: asercja, że accept queue odrzuca nadmiar i TCB wracają do puli.
- [ ] **J2.** `/proc/net/{tcp,udp}` — zweryfikować, że serwisy nasłuchujące pokazują się ze
  stanem `0A` (LISTEN) i że `netstat`-owy format dalej parsuje się wzorcem Linuksa.
- [ ] **J3.** `docs/networking.md` — przepisać sekcje: §5 (DHCP zamiast statyki + fallback),
  §9 (serwisy: inetd/telnetd/httpd + narzędzia), §10 (tabela różnic — usunąć wiersze, które
  przestały być różnicami: TCP options, DHCP, AF_UNIX/AF_PACKET, services), §11 (nowe stałe
  z FAZY B), §13 (zaktualizować out-of-scope: zostaje TLS, IPv6, offload).
- [ ] **J4.** Pełny sweep: `make test` (cel: utrzymać ≥90% agregatu), `make check-arch`,
  QEMU pełny scenariusz (boot→DHCP→ping wp.pl→wget→telnet z hosta→curl z hosta→traceroute→
  cat /proc/net/*) bez faultów; e2fsck-clean.
- [ ] **J5.** Commit: `docs+test: networking follow-up complete` + wpis w progress-logu
  (`2026-06-12-net-dociagniecia-progress.md`, format jak plan bazowy).

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
