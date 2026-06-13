# NanOS Networking

A complete, internet-proven IPv4 stack: PCI → e1000 NIC → Ethernet/ARP → IPv4 (routing,
fragmentation) → ICMP → UDP/RAW/TCP → BSD sockets → a Linux-i386 socket syscall ABI, with a libc
DNS resolver on top. The whole stack runs **unmodified GNU `ping` and `wget`** against the real
internet, and exposes Linux-format `/proc/net`.

This document describes the runtime architecture. The phase-by-phase build log and per-phase
verification live in `../superpowers/plans/2026-06-12-networking.md` (the spec) and
`…-networking-progress.md` (outcomes). For the filesystem/VFS side see `filesystem.md`.

---

## 1. Design principles

Three decisions run through the whole stack.

### Machine-independent (MI), host-tested
The entire stack under `net/` is **machine-independent**: it never touches hardware directly,
only through the `<arch/...>` contracts and a few kernel-installed hooks (the TX function on a
`NetDevice`, the "wake the softirq thread" hook, the IRQ-guard hook, the clock hook). The only
**machine-dependent (MD)** networking code is:

- `arch/x86/io/pci_x86.cpp` — PCI config space access (ports `0xCF8/0xCFC`), behind
  `arch/include/arch/pci.h`;
- `kext/e1000/` — the NIC driver, a loadable `.nkext` module.

Because the stack is MI and all I/O is injected, it **compiles and runs natively on the macOS
host** under doctest — no QEMU needed for unit/integration tests. `make check-arch` greps the MI
dirs and fails if any x86 internal (port I/O, inline asm, `Registers`, x86 headers) leaks into
`net/`. 499 host tests, ~91% line coverage, gate the stack.

> Contrast with Linux: Linux's net stack is also portable, but the arch boundary is informal.
> Here it is a hard contract — `net/` sees hardware only through injected function pointers.

### Deferred-preemption: the stack never runs in an IRQ
The e1000 IRQ handler does the absolute minimum — pull a frame out of the DMA ring and call
`netifRx(skb)`, which enqueues the buffer onto a **backlog ring** and wakes a kernel thread. The
actual protocol processing (Ethernet → IP → TCP) happens later in the `ksoftirqd-net` thread.
This is Linux's NAPI/`ksoftirqd` model, simplified to **one softirq thread, no NAPI polling**. It
is mandated by NanOS's deferred-preemption scheduler: never switch tasks or run long work inside an
IRQ handler — only enqueue and wake.

### Static pools, no hot-path allocation
Every table is a fixed-size array in `.bss` (zeroed by the multiboot loader, so no constructor):

| Pool | Size | File |
|---|---|---|
| `NetBuf` (sk_buff) | **128** (`POOL_N`) | `net/NetBuf.cpp` |
| RX backlog | **64** (`BACKLOG`) | `net/NetDevice.cpp` |
| Net devices | **8** (`MAX_DEV`) | `net/NetDevice.cpp` |
| ARP cache | **16** (`CACHE_N`) | `net/Arp.cpp` |
| Routing table | **16** (`ROUTE_N`) | `net/Route.cpp` |
| Sockets | **64** (`SOCK_N`) | `net/Socket.cpp` |
| TCP control blocks | **16** (`TCB_N`) | `net/Tcp.cpp` |

Allocation that fails (pool exhausted, backlog full) **drops the packet** and returns null/error —
it never blocks and never panics. The backlog (64) is smaller than the pool (128) on purpose, so the
backlog is the drop point and the pool can't be exhausted by a receive flood.

> Contrast with Linux: slab-backed, dynamic. NanOS is a <3 MiB kernel with a bump allocator and no
> real `free`, so everything sizable is preallocated.

---

## 2. The layers (bottom-up)

### 2.1 PCI bus + e1000 NIC
- `kernel/Pci.cpp` (MI): bus enumeration, BAR decode (write-all-ones / read-back sizing for
  32/64-bit memory and I/O BARs), bus-master/mem/io command bits. The config-space backend is
  **injected** (`Pci::setBackend`) — the real one (`arch::pciConfigRead32/Write32`) in the kernel,
  a mock in tests — so PCI is 100% host-testable.
- `kext/e1000/e1000.cpp`: a **loadable kernel module** (not compiled into the kernel). It matches
  PCI `8086:100E` (Intel 82540EM), maps MMIO via BAR0, reads the MAC from `RAL/RAH`, and sets up
  legacy DMA: an **RX ring and a TX ring of 32 descriptors** each, plus an IRQ handler. It talks to
  the kernel only through the stable `knx_*` C ABI (`kernel/knx_net.h`): `knx_map_mmio`,
  `knx_dma_alloc`, `knx_add_net_dev`, `knx_netif_rx`. Adding another NIC = another `.nkext`
  implementing the same ABI.

### 2.2 NetDevice + the RX bottom-half
`net/NetDevice.cpp` defines the device and the receive path:

```
struct NetDevice { name[16]; mac[6]; mtu; flags; ip; netmask; broadcast;
                   tx(); drvCtx; rx/txPackets/Bytes/Errors/Dropped (uint64); };
enum NetFlags { NETIF_UP, NETIF_RUNNING, NETIF_LOOPBACK, NETIF_BROADCAST };
```

- `netifRx(skb)` (called from the driver IRQ): enqueue onto the 64-slot backlog and wake the
  softirq thread. Drops + counts `rxDropped` if the backlog is full.
- `netRxProcess()` (the softirq thread): drain the backlog and hand each frame to `ethRx`.
- Registry: `netRegister / netByName / netByIndex / netCount / netPrimary` (first UP non-loopback).

`net/Loopback.cpp` registers `lo` (127.0.0.1/8); its `tx` re-injects the frame via `netifRx`, so
loopback traffic takes exactly the same demux path as the wire — and lets the whole stack be
exercised with no hardware.

### 2.3 NetBuf — the sk_buff
`net/NetBuf.cpp`: one **linear** 2 KiB buffer with headroom and the `skb_*`-style geometry
`reserve / push / pull / put` (+ `trim`). Default headroom `NET_HEADROOM = 144` leaves room for
every header a packet grows on the way down (Ethernet + IP + TCP + options). 128-buffer pool.

> Contrast with Linux: no nonlinear skb (no page frags, no `frag_list`, no scatter-gather). One
> contiguous buffer, so the maximum packet is bounded by the buffer CAP. Simpler, smaller, enough.

### 2.4 Ethernet + ARP
- `net/Ether.cpp`: `ethRx` demuxes by ethertype to the ARP or IP handler; `ethSend` prepends the
  L2 header and pads to `ETH_MIN = 60`.
- `net/Arp.cpp`: a 16-entry cache with neighbour states **INCOMPLETE / REACHABLE / STALE** (like
  Linux's neighbour states), a pending-packet queue (`arpHold` queues an IP datagram until the
  reply arrives, then transmits it), retransmitted probes and aging (`arpTick`, driven by the net
  timer thread). Introspection: `arpLookup / arpCacheCount / arpEntryAt / arpSlots`.

### 2.5 IPv4
- `net/Ip.cpp`: full header + RFC 1071 checksum, protocol demux (`IPPROTO_ICMP=1`, `TCP=6`,
  `UDP=17`). `ipRx` validates (version, IHL, total length, checksum), rejects anything not for us
  (`acceptForUs` — this is a host, **no forwarding**), reassembles fragments, then delivers to the
  L4 handler. `ipOutput` routes, resolves the next hop via ARP, and **fragments** if the datagram
  exceeds the device MTU.
- `net/Route.cpp`: a 16-entry table, **longest-prefix match** + a default route. `routeAdd /
  routeAddDefault / routeLookup / routeByIndex`.

> Contrast with Linux: no netfilter/iptables/nftables, no policy routing, no multiple routing
> tables, no forwarding. One table, longest-prefix, host-only.

### 2.6 ICMP
`net/Icmp.cpp`: echo replies are generated **automatically in the kernel** (independent of raw
sockets — so `ping` works even from a raw socket that also sees the request). `icmpSendEcho`,
`icmpSendError` (quotes the offending IP header + 8 bytes per RFC 792; used for UDP
port-unreachable). All ICMP also goes to any `SOCK_RAW` socket.

### 2.7 Sockets + UDP + RAW
- `net/Socket.cpp`: `AF_INET`, types `SOCK_DGRAM / SOCK_STREAM / SOCK_RAW`. A socket holds the
  local/remote address+port, a 16-deep datagram receive ring (`RXQ`), a `WaitQueue` for blocking
  reads, a refcount (an fd can be dup'd/forked), and `SO_*` options. `socketEphemeralPort` hands
  out 32768–60999 (Linux's range). Registry lookups: `socketLookupUdp` (most-specific match on
  type/port/addr/peer), `socketForEachRaw`, `socketAt / socketSlots`.
- `net/Udp.cpp`: pseudo-header checksum, auto-bind of a source port on first send (like Linux),
  ICMP port-unreachable when there's no listener.
- `net/Raw.cpp`: `SOCK_RAW` for ICMP; delivers the **whole IP datagram** to the socket (Linux raw
  semantics — `ping` reads the 20-byte IP header + the ICMP message).

### 2.8 TCP — the heavy part
`net/Tcp.cpp` is a full implementation:

- **State machine** (RFC 793): CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1,
  FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT.
- **RTO** with Jacobson/Karn RTT estimation (`srtt`/`rttvar`; `RTO_INIT=1000`, `RTO_MIN=200`,
  `RTO_MAX=60000` ms), retransmission on loss.
- **Reno congestion control**: slow start, congestion avoidance, fast retransmit + fast recovery
  (`cwnd`/`ssthresh`/`dupacks`).
- **TCP options (RFC 7323/2018)**: **window scaling**, **SACK** (receiver builds blocks from the
  out-of-order buffer; sender keeps a scoreboard and skips SACKed ranges on retransmit), and
  **timestamps + PAWS** — all negotiated on the SYN, matching the Linux SYN option layout
  `[MSS, SACK-permitted, TS, NOP, wscale]`. Plus a **delayed-ACK** timer (`DELAY_ACK=40` ms,
  flushed by the 50 ms tick), a **zero-window persist** probe (`PERSIST_INIT=5000`…`PERSIST_MAX=
  60000` ms), and **`SO_KEEPALIVE`** probes.
- **Out-of-order reassembly** (`OOO_N=4` pending segments), MSS negotiated on SYN (`MSS_MAX=1460`).
- Proper close with **TIME-WAIT (2·MSL = 60 s)** and `shutdown(2)` half-close. The TCB lives in
  this module's pool and can outlive its `Socket` (an orphaned TIME-WAIT, like Linux).
- Send/recv buffers are **8 KiB** each (`SNDBUF`/`RCVBUF`); accept queue 8 deep (`ACCEPT_N`); **16
  concurrent connections** (`TCB_N`), TIME-WAIT recycled under pressure.
- Socket integration: `tcpAttach/Connect/Send/Recv/Close/Shutdown/Listen/Accept`,
  `tcpReadable/Writable/State`, `tcpSnapshot` (for `/proc/net/tcp`).

> Contrast with Linux: same headline TCP options (window scaling, SACK, timestamps/PAWS, delayed
> ACK, persist, keepalive), but **no TCP fast open, no ECN, no pluggable congestion control** (Reno
> only), and a fixed **16-connection** ceiling.

---

## 3. Concurrency model — the two kernel threads

There is no `systemd`/`inetd`/NetworkManager. The whole "network daemon" is two kernel threads,
registered in `kernel/Kernel.cpp` and driven by the deferred-preemption scheduler:

1. **`ksoftirqd-net`** (`kernel/NetCore.cpp`, task id 2): sleeps on a `WaitQueue`; `netifRx`'s wake
   hook wakes it; it drains the RX backlog through the stack. The equivalent of Linux's
   `ksoftirqd`.
2. **`net-timer`** (task id 3): every **50 ms** calls `tcpTick` (RTO/retransmit/TIME-WAIT),
   `arpTick` (cache aging), `ipReasmTick` (fragment expiry). 50 ms is well under `RTO_MIN`. The
   equivalent of Linux's various net timers, merged into one thread.

Socket blocking (a `read()` with no data) parks the caller on the socket's `WaitQueue`; the RX path
wakes it via an installed wake hook. RX/TX/timer races are guarded with `cpuIrqSave` critical
sections through the injected IRQ-guard hook (so the MI core stays scheduler/arch-free).

---

## 4. The socket syscall ABI (Linux i386 compatible)

This is what lets **unmodified Linux binaries** run. `kernel/SyscallDispatch.cpp` +
`kernel/Syscall.cpp` implement all three forms glibc/uClibc emit on i386:

- **`socketcall(102)`** — the old multiplexed entry (subcalls `SC_SOCKET=1 … SC_SENDTO=11 …`);
  this is what inetutils `ping` and `wget` go through.
- **direct syscalls** `socket(359) … shutdown(373)`;
- **`_newselect(142)`** — the 5-argument `select(2)`.

Sockets live in the process fd table next to files, so `read/write/close/poll/dup/fork` operate on
them uniformly. `sockaddr_in` is marshalled exactly like Linux i686 (family little-endian,
port/address in network order); errors are negative errno (`-ENOTSOCK`, `-EINPROGRESS`,
`-EAFNOSUPPORT`, `-EROFS`, …). The build was validated against real `strace` traces (in
`tests/fixtures/strace/`) so the syscall numbers, sockopts, and blocking semantics match.

---

## 5. Interface configuration & `/etc`

`eth0` is configured by **real DHCP at boot**: `init` runs the unmodified busybox **`udhcpc`**
over an **AF_PACKET** socket (it must work before the interface has an address), which exec's
`/disks/main/nanos/config/udhcpc.script` to apply the lease via `SIOCSIFADDR`/`SIOCSIFNETMASK` +
a default route (`SIOCADDRT`) and to rewrite `/etc/resolv.conf`. The full DISCOVER→OFFER→
REQUEST→ACK exchange is on the wire (byte-faithful to a Linux `udhcpc`). The kernel's old static
**10.0.2.15/24** config remains only as a **loud fallback** — `netBringUp()` in
`kernel/NetCore.cpp` runs it (and logs that it did) if `udhcpc` is absent or fails to configure
within 10 s. `lo` is **127.0.0.1/8** with a `127.0.0.0/8 → lo` route, so guest-local connections
(`telnet 127.0.0.1 23`) loop back instead of leaking to the gateway.

Network config files live under `/etc` (a writable tmpfs the kernel populates at boot from
`/disks/main/nanos/config/etc/`): `resolv.conf`, `hosts`, `nsswitch.conf`, `protocols`,
`services`, `inetd.conf`. The resolver and `getservbyname`/`getprotobyname` **read these files**
(the built-in tables are only a fallback when a file is absent).

**Listening services run at boot** (started by `init` after DHCP, see §9): the inetd
super-server, telnetd, and darkhttpd. The server path (`tcpListen`/`tcpAccept`) is exercised
end to end — the system is reachable from outside.

---

## 6. DNS resolver (in libc)

`user/libc-glue/resolv.c` — the resolver is in **libc, not the kernel** (musl style).
`getaddrinfo/gethostbyname/getnameinfo` read `/etc/hosts`, then query the nameserver from
`/etc/resolv.conf` over UDP/53 (with DNS name decompression in the answer). **No netlink, no
nscd** (glibc uses them but degrades without them; we never need them). **IPv4-only**:
`getaddrinfo` returns only A records; `AF_INET6` yields no addresses. `getprotobyname`/
`getservbyname` are backed by small built-in tables.

---

## 7. Introspection — `/proc/net` and `/proc/bus/pci`

`net/NetProc.cpp` renders, and `fs/SynthFs.cpp` exposes (read-only, generated on read):

- `/proc/net/dev` — per-interface RX/TX counters,
- `/proc/net/route` — the routing table,
- `/proc/net/arp` — the ARP cache,
- `/proc/net/tcp`, `/proc/net/udp`, `/proc/net/raw` — sockets/connections,
- `/proc/net/snmp` — protocol counters (`net/NetStats.cpp`),
- `/proc/bus/pci/devices` — the PCI device list.

The format matches Linux exactly, so `netstat`/`ss` can parse it: tcp/udp/raw/route print IPv4 as
the network-order bytes read little-endian (`%08X`) with ports `%04X`; arp is dotted-decimal; TCP
`st` is remapped to Linux's state numbering (ESTABLISHED=`01` … LISTEN=`0A`). SNMP counters are
incremented at the IP/ICMP/TCP/UDP RX/TX entry points; fields we don't maintain report `0` (never a
fabricated number). These files are **read-only** — there is no writable `/proc/sys/net` / sysctl.

---

## 8. Packet flow, end to end

**Receive** (e.g. a ping reply):
```
e1000 IRQ → pull descriptor from the RX DMA ring → netifRx (enqueue backlog + wake)
  → ksoftirqd-net: ethRx → demux by ethertype → ipRx (checksum, for-us?, reassembly)
    → icmpRx (auto echo-reply + raw socket) / tcpRx / udpRx
      → socketDeliver → wake the blocked read()
```

**Transmit** (e.g. a wget GET):
```
send() → tcpSend (buffer + window) → sendSeg → ipOutput (route → ARP resolve)
  → ethSend (L2 header, pad to 60) → dev->tx → e1000 TX DMA ring → wire
```

Everything except the brief IRQ runs in kernel threads; socket blocking goes through `WaitQueue`.

---

## 9. Applications

All ports are *unmodified upstream*, cross-built with the nanos-sdk (config.cache + sysroot
headers only, **no source patches**), installed in `/nanos/bin`.

**Clients** (`make ping` / `make wget`):
- **GNU inetutils `ping`** and **GNU `wget`** — `ping wp.pl` (DNS → ICMP echo); `wget http://…`
  (DNS → TCP → HTTP 200, file saved).

**Servers — the system is reachable from outside** (`make inetd` / `make httpd`; `init` starts
them after DHCP):
- **inetutils `inetd`** — the internet super-server. Built-in `echo`/`discard`/`daytime`/
  `chargen` + launches `telnet → telnetd`. Config: `/etc/inetd.conf`.
- **inetutils `telnetd`** — remote login over a kernel pty. From a host, `telnet localhost 2323`
  (hostfwd) → a real `bash` login (telnetd → `nanologin` → the account's shell). Uses pty packet
  mode (TIOCPKT) + `login_tty` controlling-terminal so job control works.
- **`darkhttpd`** — single-file HTTP/1.1 server on `:80`, serving `/apps/www`. `curl
  http://localhost:5555/` → 200; handles concurrent connections (TCB_N=16).

**Diagnostic tools** (`make inetd` builds them from the same inetutils tree):
- **`ifconfig`** (eth0/lo with the live DHCP address), **`traceroute`** (UDP TTL + ICMP
  time-exceeded), **`telnet`** client (loopback test of our own telnetd).
- In-image: `nettest`, `pingtest`, `socktest`, `tcpsrv` (a libc TCP echo server), `unixtest`.

> **Known port quirk:** inetutils' argp **short** options break against picolibc's getopt, so the
> tools are driven with **long** options (`--tries`, `--max-hop`, …) or positionals; a proper libc
> getopt fix is tracked separately. HTTPS stays out of scope until a TLS library is ported.

---

## 10. Differences from Linux (summary)

| Area | Linux | NanOS |
|---|---|---|
| Stack-in-IRQ | NAPI + ksoftirqd (polling) | one softirq thread, no NAPI |
| sk_buff | nonlinear (frags/scatter) | one linear 2 KiB buffer |
| Memory | slab, dynamic | static pools (128/64/16/16) |
| TCP options | window scaling, SACK, timestamps, delayed/persist/keepalive | MSS, window scaling, SACK, timestamps+PAWS; delayed ACK, persist & keepalive timers |
| Concurrent TCP | thousands | **16** (TIME-WAIT recycled under pressure) |
| IP version | v4 + v6 | **IPv4 only** |
| Firewall/NAT | netfilter/iptables/nftables | **none** |
| Routing | multiple tables, policy | one 16-entry longest-prefix table (incl. `127/8 → lo`) |
| Namespaces/veth/bridge | yes | **none** (single namespace) |
| AF_UNIX / AF_PACKET | full | both real: AF_UNIX stream+dgram+`socketpair`, AF_PACKET cooked+raw (used by `udhcpc`) |
| Config | netlink, iproute2 | **DHCP** (busybox `udhcpc` over AF_PACKET); ioctl-based (`SIOC*`), no netlink; static = loud fallback |
| Resolver | glibc + nscd + netlink | musl-style stub in libc, IPv4-only |
| `/proc/net` | full + writable sysctls | read-only, subset, format-compatible |
| Offload (csum/TSO/GRO) | yes | **none** (all software) |
| TLS/HTTPS | userland (OpenSSL/GnuTLS) | **none** (HTTP only) |

---

## 11. Limits & constants (quick reference)

| Constant | Value | Meaning |
|---|---|---|
| `POOL_N` | 128 | NetBuf pool |
| `BACKLOG` | 64 | RX backlog (the drop point) |
| `MAX_DEV` | 8 | registered net devices |
| `CACHE_N` | 16 | ARP cache entries |
| `ROUTE_N` | 16 | routes |
| `SOCK_N` | 64 | sockets |
| `TCB_N` | 16 | concurrent TCP connections |
| `RXQ` | 16 | datagram receive ring per socket |
| `OOO_N` | 4 | TCP out-of-order pending segments |
| `ACCEPT_N` | 8 | TCP accept queue per listener |
| `SNDBUF`/`RCVBUF` | 8192 | TCP send/recv buffers |
| `NET_HEADROOM` | 144 | NetBuf headroom for headers |
| `MSS_MAX` | 1460 | max TCP MSS |
| `RTO_MIN`/`RTO_MAX` | 200 / 60000 ms | retransmit timeout bounds |
| `MSL` | 30000 ms | TIME-WAIT = 2·MSL = 60 s |
| e1000 rings | 32 / 32 | RX / TX descriptors |

---

## 12. Testing & verification

- **Host tests** (`make test`): the whole MI stack runs natively under doctest via `RamBlockDevice`
  + loopback — checksums, ARP cache/aging, longest-prefix routing, fragmentation/reassembly, the
  DNS parser, the full TCP state machine (handshake, loss/retransmit, reordering, TIME-WAIT),
  socket demux, the `/proc/net` renderers, and **hardening** (`tests/test_net_hardening.cpp`:
  malformed IP/ICMP/UDP/TCP headers, a fragment flood, bad Ethernet/ARP — each asserts no crash and
  `netbufInUse()` back to baseline, i.e. no NetBuf leak and the pool is never exhausted). 499 tests,
  ~91% coverage; new net modules ≥90%.
- **`make check-arch`**: fails if any x86 internal leaks into the MI `net/` code.
- **pcap field-compare** (`scripts/net-capture.sh`, `scripts/ping-qemu.sh`): headless QEMU with an
  e1000 on slirp NAT + `filter-dump`; ARP/IP/ICMP/DNS/TCP frames are field-compared (with an explicit
  mask of legitimately random fields — IP ID, ISN, ephemeral ports, DNS ID) against what a
  reference Linux emits for the same operation, and `cat /proc/net/*` is screendumped. The CPU
  interrupt log is grepped for fault vectors (`v=08` triple / `v=0d` #GP / `v=0e` #PF) — every gate
  requires zero.
- **Real internet**: on this macOS host QEMU's slirp forwards ICMP, so `ping wp.pl` and
  `wget http://neverssl.com` actually reach the internet.

---

## 13. Out of scope (future work)

TLS/HTTPS (needs an OpenSSL/GnuTLS port), IPv6, netfilter/firewalling, hardware offload
(csum/TSO/GRO), writable `/proc/sys/net` sysctls, and `sshd` (TLS-class crypto — telnetd is the
honest equivalent for now). A proper **libc getopt** (so inetutils short options parse) is a
tracked libc fix. Concurrent telnet logins need **dynamic pty allocation** (the kernel has one
pty pair today — enough for a single session).

> The 2026-06-12 networking follow-up (`../superpowers/plans/2026-06-12-net-dociagniecia.md`)
> is **complete**: TCP window scaling/SACK/timestamps/delayed-ACK/persist/keepalive, a real DHCP
> client, AF_UNIX + AF_PACKET + `socketpair`, ICMP-error delivery to sockets, a fuller resolver
> (`/etc/services`, search/ndots, multi-ns, PTR), listening services (inetd/telnetd/darkhttpd)
> reachable from outside, the nettools (ifconfig/traceroute/telnet), and a right-sized window/pool
> all landed. The differences in §10 are the standing, intentional ones.
