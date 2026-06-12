# Plan: pełny stos sieciowy „jak w Linuksie" — od sterownika NIC do `ping wp.pl`

> Plan dla agenta-wykonawcy. **Niczego jeszcze nie implementujemy** — to dokument projektowy.
> Cel końcowy (definicja sukcesu): w QEMU, z czystego startu, działa **`ping wp.pl`**, gdzie `ping`
> to **prawdziwy GNU/Linux `ping`** (z `inetutils`/`iputils`) **przekompilowany przez nanos-sdk bez
> żadnych łatek logiki sieciowej** — czyli rozwiązuje `wp.pl` przez DNS, wysyła ICMP echo i pokazuje
> RTT. Dodatkowy dowód kompletności: **`wget http://example.com`** (TCP) i samodzielny lookup DNS.
>
> **Zasada naczelna (powtórzona, bo to sedno zlecenia):** robimy to **dokładnie jak Linux** — ABI
> syscalli, układy struktur, stałe, zachowanie resolvera, `/proc/net`, ioctl-e — tak, żeby
> **wprost** uruchamiać przekompilowane linuksowe (i686) aplikacje sieciowe. **Zero skrótów**
> (celowych i niecelowych), zero lenistwa, zero „atrap" TCP/DNS/DHCP. Każda warstwa ma być pełna,
> poprawna bajtowo na drucie i bramkowana host-testami + porównaniem pcap z prawdziwym Linuksem.

---

## 0. Stan obecny (diagnoza — co mamy, czego NIE mamy)

**Mamy** (fundament, na którym budujemy):
- Mechanizm **nkext** (`kbd.nkext`, `mouse.nkext`): ładowalne moduły jądra z importem-po-nazwie,
  `nkext_init()`, eksporty `knx_*` (`knx_register_irq`, `knx_add_input_dev`, `knx_malloc/free`,
  `knx_log`, `knx_uptime_us`). To wzorzec dla sterownika NIC.
- **MMU**: `mmuMapKernelMmio(phys, bytes)` (mapowanie rejestrów urządzenia), `FrameAllocator`
  (bitmapa ramek fizycznych, identycznie mapowanych w katalogu jądra → źródło buforów DMA).
- **Model urządzeń**: `BlockDevice`/`CharDevice` + `DeviceManager`; VFS (SynthFs `/proc`, RamFs
  `/tmp`, ext na `/disks/main`); warstwa fd w `kernel/Syscall.{h,cpp}` + `SyscallDispatch.cpp`.
- **Scheduler** z deferred-preemption + `WaitQueue` (blokowanie jak w pipe’ach) + zegar
  (`knx_uptime_us`, tick) — to baza pod timery TCP i blokujące sockety.
- **nanos-sdk**: toolchain `i686-nanos` + `posix-hosted-patch.sh` + mknx; udowodnione porty
  (grep/vim/bzip2) — droga do zbudowania `ping`. Izolacja ring-3 (fault = kill procesu, nie panic).

**NIE mamy** (wszystko do zrobienia w tym planie):
- **PCI** — brak dostępu do config space (`0xCF8`/`0xCFC`), brak enumeracji. Bez tego nie znajdziemy
  karty sieciowej. To FAZA 1, fundament.
- **Żadnego sterownika sieciowego**, żadnej karty skonfigurowanej w QEMU.
- **Żadnego stosu**: Ethernet/ARP/IP/ICMP/UDP/TCP, routing, neighbor cache — nic.
- **Żadnych socketów**: brak `socket/bind/connect/...`, brak `socketcall`, brak integracji z fd.
- **Brak `/etc`** (`resolv.conf`, `hosts`, `nsswitch.conf`), **brak `/proc/net/*`**, brak konfiguracji
  interfejsu (DHCP/statyka), brak resolvera DNS w libc.
- **SDK**: brak linuksowo-zgodnych nagłówków `<sys/socket.h>`, `<netinet/*>`, `<arpa/inet.h>`,
  `<netdb.h>`, `<net/if.h>` o **dokładnych** układach struktur Linux i686.

---

## 1. Architektura docelowa (mapa warstw — „jak w Linuksie")

```
  userland (przekompilowane apki Linuxa: ping, wget, nslookup, ...)
    │  BSD socket API  (socket/bind/connect/sendto/recvfrom/sendmsg/recvmsg/...)
    │  resolver w libc (getaddrinfo/gethostbyname -> /etc/resolv.conf, /etc/hosts) — UDP/53
    ▼
  ── syscall ABI (DOKŁADNIE Linux i686) ────────────────────────────────────────────
    │  socketcall(2)=102 (demux) ORAZ bezpośrednie syscalle (socket=359, bind=361, ...)
    │  sockety są fd: read/write/close/poll/select/epoll/fcntl/ioctl działają na nich
    ▼
  KERNEL (MI — host-testowalny):
    socket layer        struct socket/sock, PCB (tablice gniazd), kolejki sk_buff, SO_* opcje
      ├── TCP           pełna maszyna stanów, okna, RTO/retransmisja, congestion (Reno), TIME-WAIT
      ├── UDP           porty, demux (proto,local,remote), datagram bufory
      └── RAW           SOCK_RAW (ICMP dla ping/traceroute)
    transport ▲ demux po proto/porcie
    IPv4                nagłówek+suma, routing (longest-prefix + default), frag/reasm, TTL, demux proto
      └── ICMP          echo req/reply, dest-unreachable, time-exceeded
    ARP / neighbor      cache (IP↔MAC), request/reply, kolejkowanie pakietów czekających na ARP
    Ethernet            ramkowanie (dst/src/ethertype), demux IP/ARP
    net device          struct net_device-like (MTU, MAC, tx(), statystyki), rx-path (netif_rx)
    net bottom-half     rx z IRQ -> kolejka -> wątek/softirq jądra (NIE w IRQ); timery TCP z zegara
    loopback (lo)       127.0.0.1/8
  ── kext contract (poszerzony) ────────────────────────────────────────────────────
    knx_pci_*, knx_map_mmio, knx_dma_alloc, knx_add_net_dev, knx_netif_rx, knx_register_irq
    ▼
  ARCH/x86 (MD): PCI config I/O (0xCF8/0xCFC), e1000 nkext (MMIO BAR, DMA ringi, IRQ)
    ▼
  QEMU: -netdev user (NAT 10.0.2/24, gw .2, dns .3, dhcp .15) + -device e1000 + filter-dump (pcap)
```

**Podział MI/MD (twardy inwariant, jak reszta NanOS):** CAŁY stos (Ethernet→TCP, sockety, routing,
ARP) jest **machine-independent** i host-testowalny przez **mock NetDevice** (wstrzykujemy ramki,
sprawdzamy bajt-w-bajt co wychodzi). **MD** są tylko: dostęp PCI (port I/O) i sterownik e1000
(MMIO+DMA+IRQ) — w `arch/x86/`. `make check-arch` musi pozostać czysty (żadnego x86 w stosie).

**Wybór karty: Intel e1000 (82540EM, PCI 8086:100E).** Powód: to **prawdziwy** sprzęt z kanonicznym
linuksowym sterownikiem (MMIO BAR0, EEPROM z MAC, deskryptorowe ringi RX/TX, legacy IRQ) — uczy
prawdziwego DMA i jest idealnie wspierany przez QEMU. (Alternatywa `virtio-net` = prościej, ale mniej
„prawdziwy sprzęt"; `rtl8139` = prostszy DMA. Trzymamy e1000 dla wierności; virtio jako ewentualny
plan B udokumentowany, NIE domyślny.)

**Wybór ABI socketów: OBA warianty Linux i686.** Stare glibc/i386 woła `socketcall(2)` (nr 102) z
podnumerem + wskaźnikiem do argumentów; nowsze jądra/biblioteki mają bezpośrednie syscalle
(`socket`=359, `bind`=361, `connect`=362, ...). Żeby **wprost** uruchamiać dowolne przekompilowane
binarki, implementujemy **oba**: demux `socketcall` i komplet bezpośrednich. Layouty struktur
(`sockaddr_in`, `in_addr`, `msghdr`, `iovec`, `cmsghdr`, `ifreq`, `addrinfo`) = **dokładnie** Linux
i686 (rozmiary, offsety, kolejność pól, network byte order).

---

## 2. Fazy (każda: pomiar/spec → implementacja → host-testy → pcap byte-compare → QEMU → commit)

Bramka „bez skrótów" dla KAŻDEJ fazy: (a) host-testy logiki MI ≥90% pokrycia nowych modułów;
(b) **porównanie pcap** — zrzut z `filter-dump` (QEMU) lub z host-testu rozkodować `tcpdump`/Scapy i
**porównać bajtowo** z tym, co generuje prawdziwy Linux dla tej samej operacji; (c) `make check-arch`
czysty; (d) QEMU bez `v=08/0d/0e`. Żadna faza nie jest „done" dopóki bajty na drucie ≠ Linux.

---

### FAZA 0 — sieć w QEMU + harness obserwacji drutu (NAJPIERW narzędzia)
- QEMU run: dodać `-netdev user,id=n0` + `-device e1000,netdev=n0` (i wariant z `hostfwd` do testów
  serwera). User-net to NAT: host `10.0.2.2` (gateway), `10.0.2.3` (DNS forwarder), DHCP wydaje
  `10.0.2.15`. Internet wychodzi przez NAT hosta → `ping wp.pl` realnie wyjdzie w świat.
- **Pcap dump**: `-object filter-dump,id=d0,netdev=n0,file=/tmp/nanos.pcap` — KAŻDY pakiet RX/TX ląduje
  w pcap. Na hoście `tcpdump -r /tmp/nanos.pcap -v` / Wireshark / Scapy do bajt-po-bajcie porównania.
- Skrypt `scripts/net-capture.sh` (boot + scenariusz + zrzut pcap + dekod). To nasz „złoty standard"
  poprawności — bo bajty na drucie nie kłamią.
- Makefile: opcjonalne `make run-net` (run z NIC + pcap). Cel: zanim cokolwiek napiszemy, umieć
  **zobaczyć i porównać** każdy bajt.

### FAZA 1 — magistrala PCI (fundament: znaleźć kartę)
- `arch/x86/` (MD): dostęp do config space przez porty `0xCF8` (address) / `0xCFC` (data) —
  `pciConfigRead8/16/32`, `pciConfigWrite*`. **MI** kontrakt `<arch/pci.h>` (jak inne `<arch/...>`).
- **MI** rdzeń `kernel/Pci` (host-testowalny przez mock config-space): enumeracja bus/dev/func
  (0..255 / 0..31 / 0..7), odczyt vendor/device/class/subclass, dekod **BAR-ów** (MMIO vs I/O,
  rozmiar przez write-all-ones/read-back), linia IRQ, bus-mastering enable (Command reg bit 2).
- Eksport do kextów: `knx_pci_find(vendor,device)`, `knx_pci_bar(dev,n)`, `knx_pci_irq(dev)`,
  `knx_pci_enable_bus_master(dev)` — w `kexports.def` + `KernelExports`.
- `/proc/bus/pci` (lspci-podobnie) — drobiazg, ale „jak w Linuksie" i pomaga w debug.
- **Weryfikacja**: host-test enumeracji na mocku; QEMU log enumeracji (znajdź 8086:100E); `lspci`
  na hoście dla porównania listy urządzeń.

### FAZA 2 — sterownik e1000 jako `e1000.nkext` (MD, jedyny duży kawałek arch)
- PCI match `8086:100E`; map BAR0 (MMIO rejestry) przez nowy eksport `knx_map_mmio(phys,len)`.
- Reset (CTRL.RST), odczyt MAC z EEPROM (lub RAL/RAH), ustawienie LINK UP, multicast/promisc wg potrzeb.
- **DMA**: ringi deskryptorów RX i TX w pamięci fizycznej ciągłej — nowy eksport `knx_dma_alloc(len)`
  (ciągłe ramki z `FrameAllocator`, zwraca wirt+fiz; identyczne mapowanie w katalogu jądra).
  Deskryptory e1000 (legacy 16B), bufory 2 KiB; RDBAL/RDBAH/RDLEN/RDH/RDT, TDBAL/.../TDT; wskaźniki
  head/tail. Bus-master ON.
- IRQ: `knx_register_irq(line, handler)`; w handlerze tylko ZBIERZ przyczyny (ICR: RXT0, TXDW, LSC),
  zdejmij gotowe deskryptory RX → `knx_netif_rx(dev, buf, len)` (oddaje do bottom-half stosu), zwolnij
  TX. **Nie** rób stosu w IRQ. TX: `dev->tx(buf,len)` wpisuje deskryptor, bumpuje TDT.
- Rejestracja: `knx_add_net_dev(struct NetDevice*)` (MAC, MTU=1500, `tx` fn) → interfejs `eth0`.
- **Weryfikacja**: w QEMU karta widoczna, MAC odczytany, RX/TX surowych ramek (np. echo loopback w
  user-net), pcap pokazuje ramki o poprawnym MAC; brak desync DMA pod obciążeniem.

### FAZA 3 — abstrakcja net device + rx bottom-half + loopback (MI)
- `struct NetDevice` (MI): `name`, `mac[6]`, `mtu`, `tx(skb)`, statystyki (rx/tx pkts/bytes/errs),
  flagi (UP, RUNNING, LOOPBACK), adresy IP (lista). Rejestr w stylu `DeviceManager`.
- **sk_buff-podobny** `struct NetBuf`: bufor + head/data/tail/end, miejsce na nagłówki (push/pull),
  metadane (dev, proto, l3/l4 offsety). Pula buforów (bez per-pakiet malloc na ścieżce gorącej).
- **Bottom-half**: rx z IRQ trafia do kolejki; dedykowany **wątek jądra „ksoftirqd-net"** (albo
  miękkie przerwanie odpalane przy wyjściu z IRQ — zgodnie z deferred-preemption, BEZ przełączania w
  IRQ) zdejmuje pakiety i woła `eth_rx`. Budzenie przez `WaitQueue`.
- **lo** (loopback 127.0.0.1/8): `tx` = wrzuć z powrotem do rx. Konieczne, bo wiele apek i resolver
  korzysta z localhost; też idealne do host-testów stosu bez sprzętu.
- **Weryfikacja**: host-test: wstrzyknij ramkę do mock-dev → trafia do `eth_rx`; ping lo (po fazach
  4-7) lokalnie.

### FAZA 4 — Ethernet + ARP (MI)
- Ethernet: parse/build (dst/src MAC, ethertype 0x0800 IP / 0x0806 ARP), demux; broadcast/multicast.
- **ARP**: cache (neighbor table: IP→MAC, stany INCOMPLETE/REACHABLE/STALE jak Linux), request
  (broadcast) / reply, gratuitous ARP, kolejkowanie pakietów IP czekających na rozwiązanie MAC
  (najpierw ARP, po reply dosłać). Timeout/retry wg Linuksa.
- **Weryfikacja**: host-test ARP req/reply + cache; pcap: nasz ARP request bajt-w-bajt jak Linux
  (gw `10.0.2.2`); `/proc/net/arp`.

### FAZA 5 — IPv4 (MI)
- Nagłówek IPv4 (wersja/IHL/TOS/len/id/flags/frag-offset/TTL/proto/suma/src/dst), **suma kontrolna**
  (poprawna, host-testowana wektorami), demux po `proto` (1 ICMP / 6 TCP / 17 UDP).
- **Routing**: tablica tras (cel/maska/gateway/dev/metric), longest-prefix match, default route
  (0.0.0.0/0 → gw). Wybór next-hop → ARP → Ethernet. `ip_output`/`ip_input`.
- **Fragmentacja + reasemblacja** (pełna, jak Linux: kolejka fragmentów per (src,dst,id,proto),
  timeout). TTL decrement (forwarding wyłączony — host, nie router; ale obsługa TTL=0 → ICMP).
- Adres per-interfejs, broadcast directed, `INADDR_ANY`.
- **Weryfikacja**: host-testy sum/fragmentacji/routingu; pcap: nasz nagłówek IP identyczny jak Linux;
  `/proc/net/route`, `/proc/net/dev`.

### FAZA 6 — ICMP (MI) — pierwszy realny „ruch"
- Echo request/reply (to czego potrzebuje `ping`: odbiór echo i — przez RAW — wysyłka), dest
  unreachable (port/host/net), time exceeded (dla traceroute), echo do nas → odpowiadamy.
- **Weryfikacja**: z hosta `ping 10.0.2.15` (QEMU host→guest, jeśli skonfigurowane) odpowiada; pcap
  echo reply bajtowo jak Linux. To pierwszy dowód, że RX→stos→TX działa pełną pętlą.

### FAZA 7 — warstwa socketów + UDP + RAW (MI)
- **Socket layer**: `struct socket`/`sock`, tablice PCB (UDP/TCP/RAW), bufory odbioru/wysyłki
  (kolejki sk_buff), opcje `SO_*` (REUSEADDR, RCVBUF, SNDBUF, BROADCAST, ERROR, ...), blokowanie przez
  `WaitQueue` (jak pipe), `O_NONBLOCK`, `poll`/`select`/`epoll` readiness, integracja z **tablicą fd**
  (socket = fd: `read/write/close/fcntl/ioctl/dup`).
- **UDP**: porty, demux (proto,local-addr/port,remote), `sendto`/`recvfrom`, datagram bufory,
  pseudo-header checksum, ephemeral ports, `connect` (default peer). Niezbędne dla DNS.
- **RAW** (SOCK_RAW): dostarcz/przyjmij surowe IP/ICMP po protokole — to czego używa `ping`.
- **Weryfikacja**: host-testy demux/buforów; QEMU: ręczny klient UDP echo do `10.0.2.2`; bajtowo pcap.

### FAZA 8 — TCP (MI) — największa, „bez atrap"
- **Pełna maszyna stanów** (CLOSED/LISTEN/SYN_SENT/SYN_RCVD/ESTABLISHED/FIN_WAIT_1/2/CLOSE_WAIT/
  CLOSING/LAST_ACK/TIME_WAIT), 3-way handshake, sekwencje/ACK, **przesuwne okno**, retransmisja
  (RTO z estymacją RTT Jacobson/Karn), **kontrola przeciążenia** (slow start + congestion avoidance,
  Reno: fast retransmit/recovery), delayed ACK, Nagle (+`TCP_NODELAY`), opcje (MSS; window scaling i
  SACK opcjonalnie, ale Linux-like), poprawne zamykanie + TIME-WAIT (2MSL), keepalive (opcjonalnie).
- **Timery** z zegara jądra (retransmit, delack, time-wait, persist, keepalive) — przez bottom-half,
  nie w IRQ. Bufory wysyłki/odbioru, reasemblacja out-of-order, okno odbioru = SO_RCVBUF.
- **Weryfikacja**: host-testy maszyny stanów + retransmisji (symulacja zgub/duplikatów/reordering);
  pcap: handshake/teardown/okna bajtowo zgodne z Linuksem; QEMU: pełny `wget` po HTTP (TCP/80).

### FAZA 9 — ABI syscalli socketów (DOKŁADNIE Linux i686)
- `kernel/SyscallNr.h`: numery Linux i686 — `socketcall`=102 ORAZ bezpośrednie (`socket`=359,
  `bind`=361, `connect`=362, `listen`=363, `accept4`=364, `getsockopt`=365, `setsockopt`=366,
  `getsockname`=367, `getpeername`=368, `send`/`recv`/`sendto`/`recvfrom`/`sendmsg`/`recvmsg`,
  `shutdown`, `socketpair` — pełna lista i386).
- `socketcall(call, args*)`: demux (1=socket,2=bind,...,18=accept4) — kopiuje argumenty z userspace
  wg dokładnego layoutu i woła wspólny rdzeń (ten sam co bezpośrednie syscalle).
- Sockety w **tablicy fd**: `read/write` = recv/send, `close` = zamknięcie gniazda, `poll/select/
  epoll` po readiness, `fcntl(O_NONBLOCK)`, `ioctl` (FIONREAD, SIOCATMARK), `dup`/`dup2`/CLOEXEC.
- **Net ioctl-e** (jak Linux): `SIOCGIFADDR/SIOCSIFADDR`, `SIOCGIFFLAGS`, `SIOCGIFHWADDR`,
  `SIOCGIFMTU`, `SIOCADDRT/SIOCDELRT` (trasy), `SIOCGIFCONF` — na specjalnym gnieździe (jak Linux).
- **Weryfikacja**: host-testy kopiowania/ABI; parytet z bezpośrednimi; binarka i686 z glibc (socketcall)
  i z musl (direct) — obie działają.

### FAZA 10 — konfiguracja interfejsu (DHCP „jak w Linuksie")
- **Klient DHCP** (pełny DISCOVER/OFFER/REQUEST/ACK, lease, opcje: IP/maska/gw/DNS/MTU) — w userlandzie,
  port `busybox udhcpc` lub `dhclient` (przez SDK) albo mały własny zgodny z RFC 2131; konfiguruje
  `eth0` (adres+maska), tablicę tras (default → gw), zapisuje **`/etc/resolv.conf`** (nameserver z DHCP).
  Statyka jako fallback (QEMU zna adresy: IP `10.0.2.15`, gw `10.0.2.2`, dns `10.0.2.3`).
- Bring-up przy boocie: `lo` UP + `eth0` UP + DHCP (jak `ifup`/`networkd`). Narzędzia `ifconfig`/`route`/
  `ip` (busybox/inetutils) — opcjonalne, ale „jak w Linuksie".
- `/etc` → symlink lub realny katalog na `/disks/main` (`resolv.conf`, `hosts`, `nsswitch.conf`,
  `services`, `protocols`).
- **Weryfikacja**: pcap DHCP bajtowo jak Linux; po boocie `eth0` ma `10.0.2.15`, default route, resolv.conf.

### FAZA 11 — resolver DNS (userland, w libc — „jak w Linuksie")
- **DNS to sprawa userlandu**: `getaddrinfo/getnameinfo/gethostbyname/res_*` czytają
  `/etc/resolv.conf` + `/etc/hosts` + `/etc/nsswitch.conf`, wysyłają zapytania UDP/53 (A/AAAA),
  parsują odpowiedzi (kompresja nazw!), cache, retry/timeout, fallback TCP/53 dla dużych odpowiedzi.
  Jądro daje tylko UDP/TCP sockety — reszta to **stub resolver** w libc.
- **SDK**: dostarczyć w sysroot **prawdziwy** resolver kompatybilny z Linuksem — albo port glibc
  `resolv`/`nss`, albo wierny minimalny (musl-style) z **dokładnym** API `<netdb.h>`. Bez skrótów:
  pełne parsowanie DNS, kompresja, wiele nameserverów, search domains.
- **Weryfikacja**: pcap: nasze zapytanie DNS dla `wp.pl` bajtowo jak Linux (`dig wp.pl`); `getaddrinfo`
  zwraca poprawne A; host-testy parsera DNS (w tym kompresja nazw, truncation→TCP).

### FAZA 12 — sieciowa powierzchnia sysroot SDK (KLUCZ do „wprost jak Linux")
- W `nanos-sdk/sysroot` (przez `posix-hosted-patch.sh`/sync-sysroot) dostarczyć **linuksowo-zgodne**
  nagłówki o **dokładnych** układach struktur i stałych i686:
  `<sys/socket.h>` (sockaddr, msghdr, cmsghdr, SOL_SOCKET, SO_*, SOCK_*, AF_*, MSG_*),
  `<netinet/in.h>` (sockaddr_in/in6, in_addr, INADDR_*, IPPROTO_*, htons/htonl), `<netinet/tcp.h>`
  (TCP_NODELAY...), `<netinet/ip.h>`/`<netinet/ip_icmp.h>` (dla ping/raw), `<arpa/inet.h>`
  (inet_pton/ntop/aton, ntoh*), `<netdb.h>` (addrinfo, hostent, getaddrinfo, EAI_*, gai_strerror),
  `<net/if.h>`/`<ifaddrs.h>` (ifreq, if_nameindex, getifaddrs), `<sys/select.h>`/`<poll.h>`/`<sys/epoll.h>`.
- **Te struktury muszą 1:1 odpowiadać temu, co jądro przyjmuje w syscallach** — inaczej skrót się zemści.
  To jest „dokładnie jak Linux": precompiled binarka i SDK-build używają identycznego ABI.
- `libc-glue`: wrappery socketcall/direct, `inet_*`, byte-order, integracja errno.
- **Weryfikacja**: skompilować trywialny program socketowy bez łatek; struktury `sizeof`/offsetof =
  Linux i686 (test porównawczy z prawdziwym nagłówkiem Linuksa).

### FAZA 13 — `ping` (cel końcowy) + drugi dowód TCP
- Port **GNU `ping`** (`inetutils` lub `iputils`) przez nanos-sdk — **bez** łatania logiki sieciowej
  (jak grep/vim: tylko mechanika SDK). `ping` używa SOCK_RAW/ICMP (lub SOCK_DGRAM ICMP), `getaddrinfo`
  dla `wp.pl`, pętli echo+RTT.
- Instalacja jak inne porty (`make ping` → `bin/ping.nxe` → `/nanos/bin`, opcjonalne).
- **Drugi dowód kompletności**: port `wget`/`curl` (TCP) — `wget http://example.com` pobiera stronę.
- **Weryfikacja końcowa (definicja sukcesu)**: w QEMU `ping wp.pl` → rozwiązuje DNS, wysyła ICMP,
  pokazuje RTT z prawdziwego internetu przez NAT; pcap pokazuje ICMP echo do realnego IP wp.pl;
  `wget` pobiera plik (TCP+DNS).

### FAZA 14 — weryfikacja całości, `/proc/net`, host-testy, hartowanie
- `/proc/net/{dev,route,arp,tcp,udp,raw,snmp}` — jak w Linuksie (apki i debug ich oczekują).
- Pełny zestaw host-testów MI: sumy kontrolne (IP/ICMP/UDP/TCP pseudo-header), ARP cache, routing
  longest-prefix, fragmentacja/reasm, parser DNS (kompresja), **maszyna stanów TCP** (handshake,
  retransmisja przy zgubie, reordering, TIME-WAIT), demux socketów. Pokrycie ≥90% (COV_PATTERNS).
- **Pcap byte-compare** jako twarda bramka na każdym poziomie: ARP, IP, ICMP, DNS, TCP handshake —
  bajty identyczne jak referencyjny Linux dla tej samej operacji.
- Hartowanie: złośliwe pakiety (krótkie/zniekształcone nagłówki, złe sumy, fragmenty-bomby, RST-flood,
  SYN bez ACK) NIE mogą wywalić jądra (izolacja + walidacja długości wszędzie); brak wycieków buforów.

---

## 3. Pliki (nowe / zmieniane) — szkic

**Nowe (MI, host-testowane):** `kernel/Pci.{h,cpp}`, `net/NetDevice.{h,cpp}`, `net/NetBuf.{h,cpp}`,
`net/Ether.{h,cpp}`, `net/Arp.{h,cpp}`, `net/Ip.{h,cpp}`, `net/Icmp.{h,cpp}`, `net/Route.{h,cpp}`,
`net/Socket.{h,cpp}`, `net/Udp.{h,cpp}`, `net/Tcp.{h,cpp}`, `net/Raw.{h,cpp}`, `net/Loopback.{h,cpp}`,
`net/NetProc.cpp` (/proc/net), `kernel/SocketSyscalls.{h,cpp}`; testy
`tests/test_pci.cpp test_checksum.cpp test_arp.cpp test_ip.cpp test_icmp.cpp test_route.cpp
test_udp.cpp test_tcp.cpp test_socket.cpp test_dns.cpp test_netdev.cpp`.

**Nowe (MD, arch/x86):** `arch/x86/io/pci_x86.cpp` (+ `arch/include/arch/pci.h`),
`kext/e1000/e1000.cpp` (+ `e1000.nkext`).

**Zmiany:** `kernel/SyscallNr.h` + `SyscallDispatch.cpp` + `Syscall.{h,cpp}` (socketcall + direct,
sockety w fd, net-ioctl), `kernel/KernelExports.cpp` + `kexports.def` (`knx_pci_*`, `knx_map_mmio`,
`knx_dma_alloc`, `knx_add_net_dev`, `knx_netif_rx`), `kernel/Kernel.cpp` (init PCI + skan NIC + lo +
bottom-half + bring-up), `fs/SynthFs` (/proc/net), Makefile (`MI_SOURCES` += `net/*`, `TEST_MODULES`/
`COV_PATTERNS`, `e1000.nkext`, `-netdev/-device/filter-dump`, `make ping`/`run-net`),
`scripts/net-capture.sh`. **SDK:** `nanos-sdk/sysroot/posix-hosted-patch.sh` (+ nagłówki net),
`libc-glue` (wrappery socket + resolver), porty `ping`/`wget`.

## 4. Inwarianty / ryzyka / kolejność

- **MI/MD**: stos = MI (`net/` host-testowalny mockiem), `check-arch` czysty; tylko PCI-IO + e1000 = MD.
- **Zero skrótów = pcap byte-compare**: dopóki nasze ARP/IP/ICMP/DNS/TCP nie są bajtowo jak Linux,
  faza nie jest skończona. To pilnuje, że nic nie jest „na niby".
- **Współbieżność**: RX z IRQ → kolejka → bottom-half/wątek (NIGDY stos w IRQ — zgodnie z
  deferred-preemption [[nanos-terminal-and-deferred-scheduler]]); timery TCP z zegara; blokowanie
  socketów przez `WaitQueue`; ostrożnie z wyścigami rx/tx/timer (sekcje krytyczne `cpuIrqSave`).
- **DMA**: ciągłe ramki fizyczne, identyczne mapowanie; bus-master; brak desync (lekcja z ATA:
  jeden deskryptor naraz, poprawne head/tail).
- **Największe ryzyka**: (1) **TCP** (maszyna stanów + retransmisja + congestion) — stąd ciężkie
  host-testy symulujące zgubę/reordering + pcap; (2) **e1000 DMA/IRQ** — stąd pcap + test obciążeniowy;
  (3) **ABI/struktury** — stąd test sizeof/offsetof vs prawdziwy Linux i686; (4) **DNS** (kompresja
  nazw) — host-test parsera. Kolejność ścisła: drut (0) → PCI (1) → NIC (2) → netdev/bh (3) → Ethernet/
  ARP (4) → IP (5) → ICMP (6) → sockety/UDP/RAW (7) → TCP (8) → ABI (9) → DHCP (10) → DNS (11) →
  sysroot (12) → ping (13) → weryfikacja (14). Data-path zanim aplikacje; pcap-parytet na każdym kroku.

## 5. Kryteria akceptacji (definicja „idealne, pełne, dokładne")
1. **`ping wp.pl`** w QEMU (prawdziwy GNU ping z SDK, bez łatek logiki) — DNS → ICMP echo → RTT z
   realnego internetu przez NAT. **Plus** `wget http://example.com` (TCP) i samodzielny lookup DNS.
2. **Bajt-w-bajt jak Linux** na drucie (pcap byte-compare): ARP, IP, ICMP, UDP, TCP handshake/teardown,
   DNS query — identyczne z referencyjnym Linuksem dla tej samej operacji.
3. **Wprost przekompilowane apki Linuxa i686 działają** bez łatania logiki sieciowej (ping/wget/
   nslookup), bo ABI syscalli + układy struktur + resolver = dokładnie Linux.
4. `make test` zielone, pokrycie nowych modułów MI ≥90%; `make check-arch` czysty; QEMU bez
   `v=08/0d/0e`; złośliwe pakiety nie wywalają jądra.
5. Commity czyste (bez wzmianki o AI), praca fazami na gałęzi.
