# Sieć w NanOS

Kompletny, sprawdzony w internecie stos IPv4: PCI → karta sieciowa e1000 → Ethernet/ARP → IPv4
(routing, fragmentacja) → ICMP → UDP/RAW/TCP → gniazda BSD → ABI wywołań systemowych gniazd
zgodne z Linuksem i386, a na wierzchu resolwer DNS w libc. Cały stos uruchamia **niezmodyfikowane
GNU `ping` i `wget`** w prawdziwym internecie i udostępnia `/proc/net` w formacie Linuksa.

Ten dokument opisuje architekturę uruchomieniową. Dziennik budowy faza po fazie oraz weryfikacja
dla każdej fazy znajdują się w `../superpowers/plans/2026-06-12-networking.md` (specyfikacja) oraz
`…-networking-progress.md` (wyniki). Stronę dotyczącą systemu plików/VFS opisuje `filesystem.md`.

---

## 1. Założenia projektowe

Trzy decyzje przewijają się przez cały stos.

### Niezależny od maszyny (MI), testowany na hoście
Cały stos w `net/` jest **niezależny od maszyny**: nigdy nie dotyka sprzętu bezpośrednio,
wyłącznie poprzez kontrakty `<arch/...>` i kilka haków instalowanych przez jądro (funkcja TX na
`NetDevice`, hak „obudź wątek softirq", hak osłony IRQ, hak zegara). Jedyny **zależny od maszyny
(MD)** kod sieciowy to:

- `arch/x86/io/pci_x86.cpp` — dostęp do przestrzeni konfiguracyjnej PCI (porty `0xCF8/0xCFC`), za
  `arch/include/arch/pci.h`;
- `kext/e1000/` — sterownik karty sieciowej, ładowalny moduł `.nkext`.

Ponieważ stos jest MI, a całe I/O jest wstrzykiwane, **kompiluje się i działa natywnie na hoście
macOS** pod doctest — żaden QEMU nie jest potrzebny do testów jednostkowych/integracyjnych.
`make check-arch` przeszukuje katalogi MI i kończy się błędem, jeśli jakikolwiek wewnętrzny element
x86 (port I/O, wstawki asemblerowe, `Registers`, nagłówki x86) przecieknie do `net/`. Stos jest
chroniony przez 499 testów na hoście i ~91% pokrycia linii.

> Porównanie z Linuksem: stos sieciowy Linuksa jest również przenośny, ale granica architektury
> jest nieformalna. Tutaj jest twardym kontraktem — `net/` widzi sprzęt wyłącznie poprzez
> wstrzyknięte wskaźniki do funkcji.

### Odroczona wywłaszczalność: stos nigdy nie działa w przerwaniu
Obsługa przerwania e1000 robi absolutne minimum — wyciąga ramkę z pierścienia DMA i woła
`netifRx(skb)`, co umieszcza bufor w **pierścieniu zaległości (backlog ring)** i budzi wątek jądra.
Faktyczne przetwarzanie protokołów (Ethernet → IP → TCP) odbywa się później, w wątku
`ksoftirqd-net`. To model NAPI/`ksoftirqd` z Linuksa, uproszczony do **jednego wątku softirq, bez
odpytywania NAPI**. Wymusza go scheduler NanOS z odroczoną wywłaszczalnością: nigdy nie przełączaj
zadań ani nie wykonuj długiej pracy wewnątrz obsługi przerwania — tylko kolejkuj i budź.

### Statyczne pule, brak alokacji na gorącej ścieżce
Każda tablica to tablica o stałym rozmiarze w `.bss` (zerowana przez loader multiboot, więc bez
konstruktora):

| Pula | Rozmiar | Plik |
|---|---|---|
| `NetBuf` (sk_buff) | **128** (`POOL_N`) | `net/NetBuf.cpp` |
| Backlog RX | **64** (`BACKLOG`) | `net/NetDevice.cpp` |
| Urządzenia sieciowe | **8** (`MAX_DEV`) | `net/NetDevice.cpp` |
| Cache ARP | **16** (`CACHE_N`) | `net/Arp.cpp` |
| Tablica routingu | **16** (`ROUTE_N`) | `net/Route.cpp` |
| Gniazda | **64** (`SOCK_N`) | `net/Socket.cpp` |
| Bloki kontrolne TCP | **16** (`TCB_N`) | `net/Tcp.cpp` |

Alokacja, która się nie powiedzie (wyczerpana pula, pełny backlog), **odrzuca pakiet** i zwraca
null/błąd — nigdy się nie blokuje i nigdy nie panikuje. Backlog (64) jest celowo mniejszy niż pula
(128), tak aby to backlog był punktem odrzucenia, a puli nie dało się wyczerpać zalewem odbioru.

> Porównanie z Linuksem: oparty na slabach, dynamiczny. NanOS to jądro <3 MiB z alokatorem typu
> bump i bez prawdziwego `free`, więc wszystko, co większe, jest prealokowane.

---

## 2. Warstwy (od dołu do góry)

### 2.1 Magistrala PCI + karta e1000
- `kernel/Pci.cpp` (MI): enumeracja magistrali, dekodowanie BAR (wpis samych jedynek / odczyt
  zwrotny do określenia rozmiaru dla 32/64-bitowych BAR-ów pamięci i I/O), bity poleceń
  bus-master/mem/io. Backend przestrzeni konfiguracyjnej jest **wstrzykiwany** (`Pci::setBackend`) —
  prawdziwy (`arch::pciConfigRead32/Write32`) w jądrze, mock w testach — więc PCI jest w 100%
  testowalne na hoście.
- `kext/e1000/e1000.cpp`: **ładowalny moduł jądra** (nie wkompilowany w jądro). Dopasowuje się do
  PCI `8086:100E` (Intel 82540EM), mapuje MMIO przez BAR0, odczytuje MAC z `RAL/RAH` i ustawia
  legacy DMA: **pierścień RX i pierścień TX po 32 deskryptory** każdy, plus obsługa przerwania.
  Rozmawia z jądrem wyłącznie poprzez stabilne C ABI `knx_*` (`kernel/knx_net.h`): `knx_map_mmio`,
  `knx_dma_alloc`, `knx_add_net_dev`, `knx_netif_rx`. Dodanie kolejnej karty = kolejny `.nkext`
  implementujący to samo ABI.

### 2.2 NetDevice + dolna połowa odbioru (RX bottom-half)
`net/NetDevice.cpp` definiuje urządzenie i ścieżkę odbioru:

```
struct NetDevice { name[16]; mac[6]; mtu; flags; ip; netmask; broadcast;
                   tx(); drvCtx; rx/txPackets/Bytes/Errors/Dropped (uint64); };
enum NetFlags { NETIF_UP, NETIF_RUNNING, NETIF_LOOPBACK, NETIF_BROADCAST };
```

- `netifRx(skb)` (wołane z przerwania sterownika): umieszcza w 64-elementowym backlogu i budzi wątek
  softirq. Odrzuca i zlicza `rxDropped`, jeśli backlog jest pełny.
- `netRxProcess()` (wątek softirq): opróżnia backlog i przekazuje każdą ramkę do `ethRx`.
- Rejestr: `netRegister / netByName / netByIndex / netCount / netPrimary` (pierwsze UP nie będące
  loopbackiem).

`net/Loopback.cpp` rejestruje `lo` (127.0.0.1/8); jego `tx` ponownie wstrzykuje ramkę przez
`netifRx`, więc ruch loopback przechodzi dokładnie tą samą ścieżką demultipleksacji co ruch z kabla
— i pozwala przećwiczyć cały stos bez sprzętu.

### 2.3 NetBuf — sk_buff
`net/NetBuf.cpp`: jeden **liniowy** bufor 2 KiB z headroomem i geometrią w stylu `skb_*`
`reserve / push / pull / put` (+ `trim`). Domyślny headroom `NET_HEADROOM = 144` zostawia miejsce na
każdy nagłówek, który pakiet zyskuje w drodze w dół (Ethernet + IP + TCP + opcje). Pula 128 buforów.

> Porównanie z Linuksem: brak nieliniowych skb (brak fragmentów stronicowych, brak `frag_list`, brak
> scatter-gather). Jeden ciągły bufor, więc maksymalny pakiet jest ograniczony pojemnością bufora
> CAP. Prościej, mniej, wystarczająco.

### 2.4 Ethernet + ARP
- `net/Ether.cpp`: `ethRx` demultipleksuje wg ethertype do obsługi ARP lub IP; `ethSend` dokłada na
  początku nagłówek L2 i dopełnia do `ETH_MIN = 60`.
- `net/Arp.cpp`: 16-elementowy cache ze stanami sąsiada **INCOMPLETE / REACHABLE / STALE** (jak stany
  sąsiada w Linuksie), kolejka pakietów oczekujących (`arpHold` kolejkuje datagram IP do nadejścia
  odpowiedzi, po czym go nadaje), retransmitowane sondy i starzenie (`arpTick`, napędzane przez wątek
  timera sieciowego). Introspekcja: `arpLookup / arpCacheCount / arpEntryAt / arpSlots`.

### 2.5 IPv4
- `net/Ip.cpp`: pełny nagłówek + suma kontrolna wg RFC 1071, demultipleksacja protokołu
  (`IPPROTO_ICMP=1`, `TCP=6`, `UDP=17`). `ipRx` waliduje (wersja, IHL, długość całkowita, suma
  kontrolna), odrzuca wszystko, co nie jest dla nas (`acceptForUs` — to jest host, **brak
  forwardingu**), składa fragmenty z powrotem, po czym dostarcza do obsługi L4. `ipOutput` routuje,
  rozwiązuje następny skok przez ARP i **fragmentuje**, jeśli datagram przekracza MTU urządzenia.
- `net/Route.cpp`: 16-elementowa tablica, **dopasowanie najdłuższego prefiksu** + trasa domyślna.
  `routeAdd / routeAddDefault / routeLookup / routeByIndex`.

> Porównanie z Linuksem: brak netfilter/iptables/nftables, brak routingu opartego na regułach, brak
> wielu tablic routingu, brak forwardingu. Jedna tablica, najdłuższy prefiks, tylko host.

### 2.6 ICMP
`net/Icmp.cpp`: odpowiedzi echo są generowane **automatycznie w jądrze** (niezależnie od surowych
gniazd — więc `ping` działa nawet z surowego gniazda, które też widzi żądanie). `icmpSendEcho`,
`icmpSendError` (cytuje obrażający nagłówek IP + 8 bajtów wg RFC 792; używane przy UDP
port-unreachable). Cały ICMP trafia też do każdego gniazda `SOCK_RAW`.

### 2.7 Gniazda + UDP + RAW
- `net/Socket.cpp`: `AF_INET`, typy `SOCK_DGRAM / SOCK_STREAM / SOCK_RAW`. Gniazdo trzyma
  lokalny/zdalny adres+port, 16-elementowy pierścień odbioru datagramów (`RXQ`), `WaitQueue` dla
  blokujących odczytów, licznik referencji (fd może zostać zduplikowane/sforkowane) oraz opcje
  `SO_*`. `socketEphemeralPort` rozdaje 32768–60999 (zakres Linuksa). Wyszukiwania w rejestrze:
  `socketLookupUdp` (najbardziej szczegółowe dopasowanie po typie/porcie/adresie/peerze),
  `socketForEachRaw`, `socketAt / socketSlots`.
- `net/Udp.cpp`: suma kontrolna z pseudonagłówkiem, automatyczne dowiązanie portu źródłowego przy
  pierwszym wysłaniu (jak w Linuksie), ICMP port-unreachable, gdy nie ma nasłuchującego.
- `net/Raw.cpp`: `SOCK_RAW` dla ICMP; dostarcza **cały datagram IP** do gniazda (semantyka raw z
  Linuksa — `ping` odczytuje 20-bajtowy nagłówek IP + komunikat ICMP).

### 2.8 TCP — ciężka część
`net/Tcp.cpp` to pełna implementacja:

- **Maszyna stanów** (RFC 793): CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1,
  FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT.
- **RTO** z estymacją RTT Jacobsona/Karna (`srtt`/`rttvar`; `RTO_INIT=1000`, `RTO_MIN=200`,
  `RTO_MAX=60000` ms), retransmisja przy stracie.
- **Kontrola przeciążenia Reno**: slow start, congestion avoidance, fast retransmit + fast recovery
  (`cwnd`/`ssthresh`/`dupacks`).
- **Opcje TCP (RFC 7323/2018)**: **window scaling**, **SACK** (odbiorca buduje bloki z bufora
  out-of-order; nadawca utrzymuje scoreboard i pomija zakresy potwierdzone przez SACK przy
  retransmisji) oraz **timestamps + PAWS** — wszystkie negocjowane na SYN, zgodnie z układem opcji
  SYN Linuksa `[MSS, SACK-permitted, TS, NOP, wscale]`. Plus timer **delayed-ACK** (`DELAY_ACK=40`
  ms, opróżniany przez tick co 50 ms), sonda **zero-window persist** (`PERSIST_INIT=5000`…
  `PERSIST_MAX=60000` ms) oraz sondy **`SO_KEEPALIVE`**.
- **Składanie out-of-order** (`OOO_N=4` oczekujące segmenty), MSS negocjowane na SYN
  (`MSS_MAX=1460`).
- Prawidłowe zamknięcie z **TIME-WAIT (2·MSL = 60 s)** i półzamknięciem `shutdown(2)`. TCB żyje w
  puli tego modułu i może przeżyć swoje `Socket` (osierocony TIME-WAIT, jak w Linuksie).
- Bufory wysyłania/odbioru po **8 KiB** każdy (`SNDBUF`/`RCVBUF`); kolejka accept głęboka na 8
  (`ACCEPT_N`); **16 jednoczesnych połączeń** (`TCB_N`), TIME-WAIT odzyskiwany pod presją.
- Integracja z gniazdami: `tcpAttach/Connect/Send/Recv/Close/Shutdown/Listen/Accept`,
  `tcpReadable/Writable/State`, `tcpSnapshot` (dla `/proc/net/tcp`).

> Porównanie z Linuksem: te same sztandarowe opcje TCP (window scaling, SACK, timestamps/PAWS,
> delayed ACK, persist, keepalive), ale **brak TCP fast open, brak ECN, brak wymiennej kontroli
> przeciążenia** (tylko Reno) oraz stały pułap **16 połączeń**.

---

## 3. Model współbieżności — dwa wątki jądra

Nie ma `systemd`/`inetd`/NetworkManager. Cały „demon sieciowy" to dwa wątki jądra,
zarejestrowane w `kernel/Kernel.cpp` i napędzane przez scheduler z odroczoną wywłaszczalnością:

1. **`ksoftirqd-net`** (`kernel/NetCore.cpp`, task id 2): śpi na `WaitQueue`; budzi go hak budzenia
   z `netifRx`; opróżnia backlog RX przez stos. Odpowiednik linuksowego `ksoftirqd`.
2. **`net-timer`** (task id 3): co **50 ms** woła `tcpTick` (RTO/retransmisja/TIME-WAIT), `arpTick`
   (starzenie cache'u), `ipReasmTick` (wygasanie fragmentów). 50 ms jest znacznie poniżej `RTO_MIN`.
   Odpowiednik różnych timerów sieciowych Linuksa, scalonych w jeden wątek.

Blokowanie gniazda (`read()` bez danych) parkuje wołającego na `WaitQueue` gniazda; ścieżka RX budzi
go przez zainstalowany hak budzenia. Wyścigi RX/TX/timera są chronione sekcjami krytycznymi
`cpuIrqSave` poprzez wstrzyknięty hak osłony IRQ (aby rdzeń MI pozostał wolny od schedulera/arch).

---

## 4. ABI wywołań systemowych gniazd (zgodne z Linuksem i386)

To właśnie pozwala uruchamiać **niezmodyfikowane binaria Linuksa**. `kernel/SyscallDispatch.cpp` +
`kernel/Syscall.cpp` implementują wszystkie trzy formy, które glibc/uClibc emituje na i386:

- **`socketcall(102)`** — stare multipleksowane wejście (podwołania `SC_SOCKET=1 … SC_SENDTO=11 …`);
  tędy idą inetutils `ping` i `wget`.
- **bezpośrednie wywołania systemowe** `socket(359) … shutdown(373)`;
- **`_newselect(142)`** — 5-argumentowe `select(2)`.

Gniazda żyją w tablicy fd procesu obok plików, więc `read/write/close/poll/dup/fork` działają na
nich jednolicie. `sockaddr_in` jest serializowane dokładnie jak w Linuksie i686 (rodzina
little-endian, port/adres w porządku sieciowym); błędy to ujemne errno (`-ENOTSOCK`,
`-EINPROGRESS`, `-EAFNOSUPPORT`, `-EROFS`, …). Budowa została zwalidowana wobec prawdziwych śladów
`strace` (w `tests/fixtures/strace/`), aby numery wywołań systemowych, sockopty i semantyka
blokowania się zgadzały.

---

## 5. Konfiguracja interfejsu i `/etc`

`eth0` jest konfigurowane przez **prawdziwy DHCP przy starcie**: `init` uruchamia
niezmodyfikowane busyboxowe **`udhcpc`** na gnieździe **AF_PACKET** (musi działać, zanim interfejs
ma adres), które wykonuje `/disks/main/nanos/config/udhcpc.script`, by zastosować dzierżawę przez
`SIOCSIFADDR`/`SIOCSIFNETMASK` + trasę domyślną (`SIOCADDRT`) i przepisać `/etc/resolv.conf`. Pełna
wymiana DISCOVER→OFFER→REQUEST→ACK jest na kablu (wiernie bajtowo jak linuksowe `udhcpc`). Stara
statyczna konfiguracja jądra **10.0.2.15/24** pozostaje wyłącznie jako **głośne awaryjne
rozwiązanie** — `netBringUp()` w `kernel/NetCore.cpp` ją uruchamia (i loguje, że to zrobiło), jeśli
`udhcpc` jest nieobecne lub nie skonfiguruje się w ciągu 10 s. `lo` to **127.0.0.1/8** z trasą
`127.0.0.0/8 → lo`, więc połączenia lokalne dla gościa (`telnet 127.0.0.1 23`) zawracają zamiast
wyciekać na bramę.

Pliki konfiguracji sieci leżą w `/etc` (zapisywalny tmpfs, który jądro wypełnia przy starcie z
`/disks/main/nanos/config/etc/`): `resolv.conf`, `hosts`, `nsswitch.conf`, `protocols`,
`services`, `inetd.conf`. Resolwer oraz `getservbyname`/`getprotobyname` **czytają te pliki**
(wbudowane tablice są tylko awaryjnym rozwiązaniem, gdy plik jest nieobecny).

**Usługi nasłuchujące uruchamiają się przy starcie** (startowane przez `init` po DHCP, patrz §9):
super-serwer inetd, telnetd i darkhttpd. Ścieżka serwera (`tcpListen`/`tcpAccept`) jest przećwiczona
od początku do końca — system jest osiągalny z zewnątrz.

---

## 6. Resolwer DNS (w libc)

`user/libc-glue/resolv.c` — resolwer jest w **libc, nie w jądrze** (styl musl).
`getaddrinfo/gethostbyname/getnameinfo` czytają `/etc/hosts`, a następnie odpytują serwer nazw z
`/etc/resolv.conf` przez UDP/53 (z dekompresją nazw DNS w odpowiedzi). **Brak netlinka, brak nscd**
(glibc ich używa, ale degraduje się bez nich; my nigdy ich nie potrzebujemy). **Tylko IPv4**:
`getaddrinfo` zwraca wyłącznie rekordy A; `AF_INET6` nie daje żadnych adresów. `getprotobyname`/
`getservbyname` są oparte na małych wbudowanych tablicach.

---

## 7. Introspekcja — `/proc/net` i `/proc/bus/pci`

`net/NetProc.cpp` renderuje, a `fs/SynthFs.cpp` udostępnia (tylko do odczytu, generowane przy
odczycie):

- `/proc/net/dev` — liczniki RX/TX per interfejs,
- `/proc/net/route` — tablica routingu,
- `/proc/net/arp` — cache ARP,
- `/proc/net/tcp`, `/proc/net/udp`, `/proc/net/raw` — gniazda/połączenia,
- `/proc/net/snmp` — liczniki protokołów (`net/NetStats.cpp`),
- `/proc/bus/pci/devices` — lista urządzeń PCI.

Format dokładnie odpowiada Linuksowi, więc `netstat`/`ss` mogą go sparsować: tcp/udp/raw/route
wypisują IPv4 jako bajty w porządku sieciowym odczytane little-endian (`%08X`) z portami `%04X`; arp
jest w notacji kropkowo-dziesiętnej; TCP `st` jest przemapowane na numerację stanów Linuksa
(ESTABLISHED=`01` … LISTEN=`0A`). Liczniki SNMP są inkrementowane w punktach wejścia RX/TX
IP/ICMP/TCP/UDP; pola, których nie utrzymujemy, raportują `0` (nigdy zmyślona liczba). Te pliki są
**tylko do odczytu** — nie ma zapisywalnego `/proc/sys/net` / sysctl.

---

## 8. Przepływ pakietu, od początku do końca

**Odbiór** (np. odpowiedź ping):
```
e1000 IRQ → pull descriptor from the RX DMA ring → netifRx (enqueue backlog + wake)
  → ksoftirqd-net: ethRx → demux by ethertype → ipRx (checksum, for-us?, reassembly)
    → icmpRx (auto echo-reply + raw socket) / tcpRx / udpRx
      → socketDeliver → wake the blocked read()
```

**Nadanie** (np. GET przez wget):
```
send() → tcpSend (buffer + window) → sendSeg → ipOutput (route → ARP resolve)
  → ethSend (L2 header, pad to 60) → dev->tx → e1000 TX DMA ring → wire
```

Wszystko poza krótkim przerwaniem działa w wątkach jądra; blokowanie gniazda przechodzi przez
`WaitQueue`.

---

## 9. Aplikacje

Wszystkie porty są *niezmodyfikowane z upstreamu*, budowane krzyżowo za pomocą nanos-sdk (tylko
config.cache + nagłówki sysroot, **bez łatek na źródła**), instalowane w `/nanos/bin`.

**Klienci** (`make ping` / `make wget`):
- **GNU inetutils `ping`** i **GNU `wget`** — `ping wp.pl` (DNS → echo ICMP); `wget http://…`
  (DNS → TCP → HTTP 200, plik zapisany).

**Serwery — system jest osiągalny z zewnątrz** (`make inetd` / `make httpd`; `init` uruchamia je
po DHCP):
- **inetutils `inetd`** — internetowy super-serwer. Wbudowane `echo`/`discard`/`daytime`/
  `chargen` + uruchamia `telnet → telnetd`. Konfiguracja: `/etc/inetd.conf`.
- **inetutils `telnetd`** — zdalne logowanie przez pty jądra. Z hosta `telnet localhost 2323`
  (hostfwd) → prawdziwe logowanie do `bash` (telnetd → `nanologin` → powłoka konta). Używa trybu
  pakietowego pty (TIOCPKT) + terminala sterującego `login_tty`, aby działała kontrola zadań.
- **`darkhttpd`** — jednoplikowy serwer HTTP/1.1 na `:80`, serwujący `/apps/www`. `curl
  http://localhost:5555/` → 200; obsługuje połączenia jednoczesne (TCB_N=16).

**Narzędzia diagnostyczne** (`make inetd` buduje je z tego samego drzewa inetutils):
- **`ifconfig`** (eth0/lo z aktualnym adresem DHCP), **`traceroute`** (UDP TTL + ICMP
  time-exceeded), klient **`telnet`** (test loopback naszego własnego telnetd).
- W obrazie: `nettest`, `pingtest`, `socktest`, `tcpsrv` (serwer echo TCP w libc), `unixtest`.

> **Znana osobliwość portu:** **krótkie** opcje argp w inetutils nie działają z getopt picolibc,
> więc narzędzia są obsługiwane **długimi** opcjami (`--tries`, `--max-hop`, …) lub argumentami
> pozycyjnymi; właściwa poprawka getopt w libc jest śledzona osobno. HTTPS pozostaje poza zakresem,
> dopóki nie zostanie sportowana biblioteka TLS.

---

## 10. Różnice względem Linuksa (podsumowanie)

| Obszar | Linux | NanOS |
|---|---|---|
| Stos w IRQ | NAPI + ksoftirqd (odpytywanie) | jeden wątek softirq, bez NAPI |
| sk_buff | nieliniowy (fragmenty/scatter) | jeden liniowy bufor 2 KiB |
| Pamięć | slab, dynamiczna | statyczne pule (128/64/16/16) |
| Opcje TCP | window scaling, SACK, timestamps, delayed/persist/keepalive | MSS, window scaling, SACK, timestamps+PAWS; timery delayed ACK, persist i keepalive |
| Jednoczesne TCP | tysiące | **16** (TIME-WAIT odzyskiwany pod presją) |
| Wersja IP | v4 + v6 | **tylko IPv4** |
| Firewall/NAT | netfilter/iptables/nftables | **brak** |
| Routing | wiele tablic, reguły | jedna 16-elementowa tablica najdłuższego prefiksu (w tym `127/8 → lo`) |
| Przestrzenie nazw/veth/bridge | tak | **brak** (jedna przestrzeń nazw) |
| AF_UNIX / AF_PACKET | pełne | oba prawdziwe: AF_UNIX stream+dgram+`socketpair`, AF_PACKET cooked+raw (używane przez `udhcpc`) |
| Konfiguracja | netlink, iproute2 | **DHCP** (busyboxowe `udhcpc` przez AF_PACKET); oparte na ioctl (`SIOC*`), bez netlinka; statyka = głośne awaryjne rozwiązanie |
| Resolwer | glibc + nscd + netlink | namiastka w stylu musl w libc, tylko IPv4 |
| `/proc/net` | pełne + zapisywalne sysctl | tylko do odczytu, podzbiór, zgodne formatem |
| Offload (csum/TSO/GRO) | tak | **brak** (wszystko programowo) |
| TLS/HTTPS | userland (OpenSSL/GnuTLS) | **brak** (tylko HTTP) |

---

## 11. Limity i stałe (krótka ściąga)

| Stała | Wartość | Znaczenie |
|---|---|---|
| `POOL_N` | 128 | pula NetBuf |
| `BACKLOG` | 64 | backlog RX (punkt odrzucenia) |
| `MAX_DEV` | 8 | zarejestrowane urządzenia sieciowe |
| `CACHE_N` | 16 | wpisy cache'u ARP |
| `ROUTE_N` | 16 | trasy |
| `SOCK_N` | 64 | gniazda |
| `TCB_N` | 16 | jednoczesne połączenia TCP |
| `RXQ` | 16 | pierścień odbioru datagramów per gniazdo |
| `OOO_N` | 4 | oczekujące segmenty TCP out-of-order |
| `ACCEPT_N` | 8 | kolejka accept TCP per nasłuchujący |
| `SNDBUF`/`RCVBUF` | 8192 | bufory wysyłania/odbioru TCP |
| `NET_HEADROOM` | 144 | headroom NetBuf na nagłówki |
| `MSS_MAX` | 1460 | maksymalne MSS TCP |
| `RTO_MIN`/`RTO_MAX` | 200 / 60000 ms | granice timeoutu retransmisji |
| `MSL` | 30000 ms | TIME-WAIT = 2·MSL = 60 s |
| pierścienie e1000 | 32 / 32 | deskryptory RX / TX |

---

## 12. Testowanie i weryfikacja

- **Testy na hoście** (`make test`): cały stos MI działa natywnie pod doctest przez
  `RamBlockDevice` + loopback — sumy kontrolne, cache/starzenie ARP, routing najdłuższego prefiksu,
  fragmentacja/składanie, parser DNS, pełna maszyna stanów TCP (handshake, strata/retransmisja,
  zmiana kolejności, TIME-WAIT), demultipleksacja gniazd, renderery `/proc/net` oraz **hartowanie**
  (`tests/test_net_hardening.cpp`: zniekształcone nagłówki IP/ICMP/UDP/TCP, zalew fragmentów, błędny
  Ethernet/ARP — każdy stwierdza brak awarii i `netbufInUse()` z powrotem na poziomie bazowym, tj.
  brak wycieku NetBuf i pula nigdy nie jest wyczerpana). 499 testów, ~91% pokrycia; nowe moduły
  sieciowe ≥90%.
- **`make check-arch`**: kończy się błędem, jeśli jakikolwiek wewnętrzny element x86 przecieknie do
  kodu MI `net/`.
- **porównanie pól pcap** (`scripts/net-capture.sh`, `scripts/ping-qemu.sh`): bezgłowy QEMU z e1000
  na slirp NAT + `filter-dump`; ramki ARP/IP/ICMP/DNS/TCP są porównywane pole po polu (z jawną maską
  prawomocnie losowych pól — IP ID, ISN, porty efemeryczne, DNS ID) z tym, co referencyjny Linux
  emituje dla tej samej operacji, a `cat /proc/net/*` jest zrzucane z ekranu. Log przerwań CPU jest
  przeszukiwany pod kątem wektorów błędów (`v=08` triple / `v=0d` #GP / `v=0e` #PF) — każda bramka
  wymaga zera.
- **Prawdziwy internet**: na tym hoście macOS slirp QEMU przekazuje ICMP, więc `ping wp.pl` i
  `wget http://neverssl.com` faktycznie docierają do internetu.

---

## 13. Poza zakresem (przyszłe prace)

TLS/HTTPS (wymaga portu OpenSSL/GnuTLS), IPv6, netfilter/firewalling, offload sprzętowy
(csum/TSO/GRO), zapisywalne sysctl `/proc/sys/net` oraz `sshd` (kryptografia klasy TLS — telnetd jest
na razie uczciwym odpowiednikiem). Właściwy **getopt w libc** (aby krótkie opcje inetutils się
parsowały) to śledzona poprawka libc. Jednoczesne logowania telnet wymagają **dynamicznej alokacji
pty** (jądro ma dziś jedną parę pty — wystarczającą na jedną sesję).

> Doszlifowanie sieci z 2026-06-12 (`../superpowers/plans/2026-06-12-net-dociagniecia.md`)
> jest **ukończone**: TCP window scaling/SACK/timestamps/delayed-ACK/persist/keepalive, prawdziwy
> klient DHCP, AF_UNIX + AF_PACKET + `socketpair`, dostarczanie błędów ICMP do gniazd, pełniejszy
> resolwer (`/etc/services`, search/ndots, multi-ns, PTR), usługi nasłuchujące
> (inetd/telnetd/darkhttpd) osiągalne z zewnątrz, nettools (ifconfig/traceroute/telnet) oraz
> odpowiednio dobrane okno/pula — wszystko zostało wprowadzone. Różnice w §10 to stałe, zamierzone.
