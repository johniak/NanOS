# Networking implementation — progress log

Companion to `2026-06-12-networking.md`. One row per phase; updated as each lands. The "gate"
columns are the no-shortcuts bars from §2 of the plan (host-tests ≥90%, pcap byte-compare,
strace parity, `check-arch` clean, QEMU no `v=08/0d/0e`).

| Faza | Status | host-test | pcap | strace | QEMU | commit |
|---|---|---|---|---|---|---|
| 0 — QEMU net + pcap harness + baseline + strace | **DONE** | n/a | harness ready | fixtures captured | n/a | f094a47 |
| 1 — PCI bus | **DONE** | Pci.cpp 93.8% | n/a (no wire yet) | n/a | e1000 found @0:3.0 irq11 | — |
| 2 — e1000.nkext | **DONE** | n/a (MD) | TX frame on wire, correct MAC | n/a | eth0 up, no faults | — |
| 3 — NetDevice + bottom-half + lo | **DONE** | ≥90% all modules | (via FAZA 2 TX) | n/a | softirq + lo wired | — |
| 4 — Ethernet + ARP | **DONE** | Ether 100% / Arp 93.2% | ARP req/reply byte-exact (tcpdump) | n/a | gw resolved, RX path OK | — |
| 5 — IPv4 | **DONE** | Ip 97.4% / Route 100% | byte-exact header (host); on wire via ICMP (F6) | n/a | builds, wired | — |
| 6 — ICMP | **DONE** | Icmp 100% | echo req/reply on wire (tcpdump) | n/a | round-trip to gw OK | — |
| 7 — sockets + UDP + RAW (AF_PACKET→F10) | **DONE** | Socket/Udp/Raw ≥92% | UDP/ICMP byte-exact (host) | n/a | builds, wired | — |
| 8 — TCP | **DONE** | Tcp 92.2% | full session w/ 1.1.1.1 (tcpdump) | n/a | clean boot, no faults | — |
| 9 — socket syscall ABI | **DONE** | Syscall.cpp socket paths tested | ring-3 TCP session w/ 1.1.1.1 (tcpdump) | ABI matches strace | socktest from ring 3, no faults | — |
| 10 — iface bring-up + /etc (DHCP→follow-up) | **DONE** | n/a (boot/image) | **ping wp.pl on the wire** (tcpdump) | n/a | DNS+ICMP to wp.pl OK, no faults | — |
| 11 — DNS resolver (libc) | **DONE** | n/a (userland) | getaddrinfo(wp.pl) on the wire | matches glibc stub | nettest resolves wp.pl | — |
| 12 — net headers + libc socket glue | **DONE** | n/a (userland) | TCP to wp.pl via POSIX API | n/a | nettest: getaddrinfo+BSD OK | — |
| 13 — ping (inetutils) + wget | **DONE** | n/a (port) | **real GNU ping `ping wp.pl`** (DNS→ICMP echo) + **real GNU wget `wget http://neverssl.com`** (DNS→TCP→HTTP 200, file saved) on the wire | unmodified upstream binaries | clean boot, no faults | — |
| 14 — /proc/net + hardening | **DONE** | NetProc 93.5% / NetStats 100%; 499 tests, agg 90.7% | n/a (read-only introspection) | Linux /proc format | `cat /proc/net/{dev,route,snmp}` + `/proc/bus/pci/devices` render, no faults | — |

## FAZA 0 — outcomes (2026-06-12)

**Harness (durable artifacts):**
- `Makefile`: `make run-net` + `NIC_OPTS` (e1000 = 82540EM = PCI 8086:100E, on `-netdev user`
  NAT, `filter-dump` → `$(PCAP)`, `hostfwd tcp::5555-:80`). QEMU 10.2 confirms `e1000`
  (alias `e1000-82540em`) and `filter-dump` are available.
- `scripts/net-capture.sh`: headless boot with the NIC + pcap, screendump, then a per-protocol
  `tcpdump` summary (arp/icmp/dns/dhcp/tcp/ip) + first 40 decoded frames + fault-vector summary.
  This is the wire observatory for the pcap gate.
- `tests/fixtures/strace/`: **real** straces of `inetutils ping` (numeric + hostname) and
  `wget` from a Debian Linux, plus `README.md` distilling the exact syscall/sockopt/`/etc`
  contract and the i686 syscall-number mapping. Input contract for FAZY 7–13.

**Baseline (external-risk check — slirp/ICMP on macOS):**
- Host reaches `wp.pl` (212.77.98.9, ttl 60, ~11 ms) → NAT egress to the real internet exists.
- macOS host permits ICMP (host `ping` works); QEMU 10.2 ships a modern libslirp that forwards
  ICMP echo on macOS via unprivileged ICMP datagram sockets — so guest `ping wp.pl` is *expected*
  to work. The definitive in-guest baseline (boot an i686 Linux with identical `-netdev user`)
  is deferred but cheap to run later via `net-capture.sh` against an Alpine ISO.
- **Pre-defined fallback (already in the plan §5.1):** if a later guest test shows slirp does NOT
  forward ICMP to the internet on this host, the ICMP success criterion becomes `ping 10.0.2.2`
  (slirp answers itself) + ICMP-echo confirmed in pcap, and "real internet" proof shifts to
  DNS + TCP (`wget`), which use slirp's UDP/TCP forwarding (not ICMP) and are unaffected.

**Key contract facts extracted from the straces (drive FAZY 7–13):**
- ping uses `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)` + `SO_BROADCAST`; RAW RX delivers the IP
  header too (recvfrom returned 20B IP + 64B ICMP).
- DNS is a userland stub resolver over `socket(AF_INET, SOCK_DGRAM)` to `:53`, reading
  `/etc/{nsswitch.conf,resolv.conf,hosts}` → FAZA 10 must populate `/etc`.
- wget's TCP recv uses `MSG_PEEK` → our TCP recv must honor it.
- Apps probe AF_INET6 first and fall back on `EAFNOSUPPORT`/`ENETUNREACH` → FAZA 11 IPv6 semantics.
- Our SDK resolver must NOT depend on netlink/nscd (glibc uses them but degrades gracefully;
  we ship a musl-style resolver that never needs them).

## FAZA 1 — outcomes (2026-06-12)

- `arch/include/arch/pci.h` (MD contract) + `arch/x86/io/pci_x86.cpp` (config mechanism #1,
  0xCF8/0xCFC) — the ONLY machine-dependent PCI code. `arch.mk` gains `arch/x86/io` + `pci_x86.o`.
- `kernel/Pci.{h,cpp}` (MI): config accessors (8/16-bit derived from a 32-bit injectable
  backend), `probe`/`enumerate`/`find`, BAR decode with write-all-ones/read-back sizing
  (32 & 64-bit mem + I/O BARs), bus-master/mem/io command bits. Backend is injected
  (`Pci::setBackend`) so it's 100% host-testable against a mock config space.
- `kernel/KernelExports.cpp` + `kexports.def`: `knx_pci_find/bar/bar_size/bar_is_io/irq/
  enable_bus_master/cfg_read32/cfg_write32` — the e1000 kext (FAZA 2) binds its device through these.
- `kernel/Kernel.cpp`: installs the arch backend + `pciScanReport()` at boot, before kext load.
- `tests/test_pci.cpp`: 8 cases (empty bus, e1000 probe + BAR decode, find-by-id, multifunction
  enumerate, single-function skip, 64-bit BAR, bus-master RMW, 8/16-bit accessors). **Pci.cpp
  93.8% line coverage**; full suite green at 91.5% aggregate; `check-arch` clean.
- **QEMU verification:** boot log shows `PCI: 6 device(s); e1000 8086:100E @ 0:3.0
  BAR0=0xFEB80000 irq=11` — the NIC the driver phase will bind to is found. No `v=08/0d/0e`.
- Deferred to FAZA 14: `/proc/bus/pci` (needs a SynthFs generator; bundled with `/proc/net`).

## FAZA 3 — MI core outcomes (2026-06-12)

- `net/Net.{h,cpp}`: byte-order helpers + endianness-independent wire rd/wr + the Internet
  checksum (RFC 1071, with accumulate/finish for pseudo-header chaining). 100% cov; verified
  against the canonical IPv4-header vector (0xb861) and carry-folding.
- `net/NetBuf.{h,cpp}`: the sk_buff — one 2 KiB buffer with reserve/push/pull/put geometry +
  a 128-buffer static pool (no hot-path malloc; alloc returns null on exhaustion, never blocks).
- `net/NetDevice.{h,cpp}`: device struct (mac/mtu/flags/ip/stats/tx) + registry + the RX
  bottom-half — `netifRx` (enqueue from driver IRQ + wake) and `netRxProcess` (drain in a
  softirq thread, NOT in IRQ) split exactly per deferred-preemption. Backlog (64) < pool (128)
  so the backlog is the drop point. Wake + IRQ-guard are kernel-installed hooks, so the core
  is arch/scheduler-free and 100% host-testable.
- `net/Loopback.{h,cpp}`: `lo` (127.0.0.1/8) whose tx re-injects into netifRx — same demux as
  the wire, hardware-free stack testing.
- `tests/test_checksum.cpp` + `tests/test_netdev.cpp`: NetBuf geometry/pool, registry, RX
  enqueue→drain, backlog overflow→rxDropped, tx success/error/no-fn paths, IRQ-guard pairing,
  loopback round-trip. **All new modules ≥91% line cov**; 424 tests green; kernel links these
  MI objects; `check-arch` clean.
- The kernel-side bring-up (lo, the softirq kthread, the knx_netif_rx/map_mmio/dma_alloc/
  add_net_dev exports) lands with FAZA 2, where the e1000 driver needs them to be verifiable.

## FAZA 2 — e1000 driver + kernel net glue (2026-06-12)

- `kernel/knx_net.h`: the stable C ABI between a NIC kext and the kernel — `KnxNetDev`
  descriptor + `knx_map_mmio` / `knx_dma_alloc` / `knx_add_net_dev` / `knx_netif_rx`. A kext
  never sees NetDevice/NetBuf internals; it exchanges flat frame bytes.
- `kernel/NetCore.{h,cpp}`: the glue (MI-clean — only `<arch/...>` contracts). Implements the
  four net exports, the KnxNetDev→NetDevice bridge (kext tx wrapped so the device owns the
  skb), and `netCoreInit()` which installs the wake + IRQ-guard hooks, creates `lo`, and spawns
  the `ksoftirqd-net` softirq thread (id 2) that drains the RX backlog outside IRQ. Wired into
  `Kernel.cpp` (`registerKthread(netCoreInit(), "ksoftirqd-net")`) and `kexports.def`/the export
  table. DMA: `knx_dma_alloc` hands out single identity-mapped frames (rings 512 B + 2 KiB
  buffers all fit ≤4 KiB, so contiguity is trivial; phys == virt).
- `kext/e1000/e1000.cpp` (`e1000.nkext`, the only large MD piece): PCI match 8086:100E, BAR0
  MMIO map, CTRL.RST reset, MAC from RAL0/RAH0 (EEPROM auto-loaded), legacy 16-byte RX/TX
  descriptor rings (32 each) in DMA memory, RCTL/TCTL/TIPG, IRQ handler that reads ICR and
  drains completed RX descriptors to `knx_netif_rx` (copy-out, NO stack in IRQ) + hands
  descriptors back via RDT; tx copies into the ring, bumps TDT, waits DD. ndo_start_xmit
  ownership: the device tx always consumes the skb (netTransmit never double-frees).
- `net/NetBuf`: added an IRQ guard on the pool (alloc runs in the e1000 IRQ, free in the
  softirq thread) so the free-list can't be corrupted by an IRQ mid-update.
- Makefile: `KEXTS += e1000`, build/link rules, installed to `/nanos/kext` (KextLoader loads it).
- **QEMU verification:** boot log `kext: e1000.nkext  e1000: eth0 up`. A diagnostic raw
  broadcast TX (since removed — ARP is the real first frame) appeared in the pcap exactly:
  `52:54:00:12:34:56 > ff:ff:ff:ff:ff:ff, ethertype 0x88b5, len 64: NANOS-TX-TEST` — proving
  the full netTransmit→kext→DMA→wire path and a correctly-read MAC. No `v=08/0d/0e`. RX is
  exercised end-to-end by the ARP reply in FAZA 4.

## FAZA 4 — Ethernet + ARP (2026-06-12)

- `net/Ether.{h,cpp}`: Ethernet II framing. `ethInit()` installs `ethRx` as the net-core input
  handler; `ethRx` parses the 14-byte header, sets `skb->protocol`, strips it, demuxes to the
  registered L3 handler (ARP now, IP in FAZA 5). `ethSend` prepends the header (src = dev MAC),
  pads to the 60-byte minimum exactly like Linux, transmits. 100% line cov.
- `net/Arp.{h,cpp}`: the neighbor cache (INCOMPLETE/REACHABLE/STALE) + request/reply + a pending
  packet queue. `arpRx` learns senders Linux-style (update any; create only for ARP-to-us or
  replies), answers requests for our IP. `arpResolve` returns a cached MAC or fires a request +
  goes INCOMPLETE; `arpHold` queues an IP packet that `arpRx` flushes (as an IP frame) on reply.
  `arpTick(now)` ages entries (REACHABLE→STALE @30 s; INCOMPLETE retransmits ≤3 @1 s then drops),
  driven traffic-wise from the softirq. 93.2% line cov.
- Wired into `netCoreInit` (ethInit + arpInit + arpSetClock(ticks)); the softirq calls arpTick.
- `tests/test_arp.cpp` (16 cases): byte-exact reply to a request-for-us, no-reply/no-cache for
  unrelated requests, reply learning, resolve miss→broadcast request (byte-exact)→no-dup→hit,
  pending-packet flush as IP, runt-frame hardening, full aging state machine, Ethernet
  IP-dispatch / unknown-ethertype / null-arg / no-pad paths. 435 host tests green; check-arch clean.
- **QEMU verification (pcap, the no-shortcuts gate):**
  `52:54:00:12:34:56 > ff:ff:ff:ff:ff:ff  ARP Request who-has 10.0.2.2 tell 10.0.2.15` (len 60,
  padded — Linux-identical) then `52:55:0a:00:02:02 > 52:54:00:12:34:56  ARP Reply 10.0.2.2
  is-at 52:55:0a:00:02:02`. A temp boot probe (since removed) ARPed the gateway and the console
  printed `[arp] gw 10.0.2.2 is at 52:55:0a:00:02:02  <-- RX path OK`, proving NIC IRQ →
  bottom-half → ethRx → arpRx → cache end-to-end. No faults.

## FAZA 5 — IPv4 + routing (2026-06-12)

- `net/Route.{h,cpp}`: the routing table — longest-prefix match + default route + metric
  tie-break. `routeLookup(dst)` returns the egress device and next-hop IP (gateway off-link,
  destination on-link). `routeAddDefault(dev, gw)` installs the on-link subnet + default. 100% cov.
- `net/Ip.{h,cpp}`: `ipRx` (installed as Ether's IP handler) validates version/IHL/length and the
  header checksum, accepts-for-us (unicast / limited+directed broadcast / loopback / unconfigured),
  reassembles fragments, and demuxes by protocol to ICMP/UDP/TCP handlers (`ipSetHandler`).
  `ipOutput(dst, proto, skb)` builds the header (src = egress IP, TTL 64, IP-id counter, valid
  checksum), routes, **fragments if it exceeds the MTU**, resolves the next-hop MAC via ARP
  (queuing through arpHold if needed), and transmits. Reassembly is bounded by the 2 KiB NetBuf
  (a buffer bound, openly stated — target traffic never fragments; MF/offset/ordering/timeout are
  fully implemented). 97.4% cov.
- NetBuf gained `saddr/daddr/ipproto` so the transport layer gets the addressing. Wired into
  `netCoreInit` (ipInit + clock) and the softirq (`ipReasmTick`).
- `tests/test_ip.cpp` (15 cases): routing (longest-prefix, on-link vs gw, replace, metric,
  del/iterate), demux with src/dst/proto, bad-checksum/runt/not-for-us drops, broadcast accept,
  `ipOutput` byte-exact header + valid checksum + routes-via-gateway, no-route drop, in-order &
  out-of-order reassembly, fragment expiry, and TX fragmentation (offsets contiguous, MF on all
  but last, each fragment's header checksum valid, fragments cover the whole payload). 446 host
  tests green, check-arch clean. On-wire IP verification lands with ICMP echo in FAZA 6.

## FAZA 6 — ICMP (2026-06-12)

- `net/Icmp.{h,cpp}`: echo request/reply (auto-reply to requests for us, keeping id/seq/data),
  destination-unreachable + time-exceeded (`icmpSendError` quotes the offending IP header + 8
  bytes via the preserved `l3`/`l4` offsets, RFC 792), `icmpSendEcho` (kernel-side ping), a raw
  hook for SOCK_RAW (FAZA 7), and an echo-reply hook. ICMP checksum over the whole message. 100% cov.
- Wired into `netCoreInit` (icmpInit registers IP proto 1).
- `tests/test_icmp.cpp` (6 cases): byte-exact echo reply on the wire (src/dst swapped, id/seq/
  data preserved, both IP and ICMP checksums valid), echo-reply handler, raw precedence,
  bad-checksum drop, `icmpSendEcho` wire format, `icmpSendError` port-unreachable quoting. 452
  host tests green, check-arch clean.
- **QEMU verification (first real round-trip):** a temp boot probe statically addressed eth0,
  added a default route, and ICMP-echoed the gateway. The pcap shows the full sequence —
  `ARP who-has 10.0.2.2` → `Reply is-at 52:55:0a:00:02:02` → `IP 10.0.2.15 > 10.0.2.2: ICMP echo
  request id 4660 seq 1` → `IP 10.0.2.2 > 10.0.2.15: ICMP echo reply` — proving ARP queue→flush,
  IP TX with routing, and the reply received + parsed (`[icmp] echo reply from 10.0.2.2 ...
  round-trip OK` on the console). Probe since removed. No faults. The whole RX/TX data path
  (NIC↔Ethernet↔ARP↔IP↔ICMP) is now proven end to end.

## FAZA 7 — socket layer + UDP + RAW (2026-06-12)

- `net/Socket.{h,cpp}`: the MI socket object (struct socket/sock) + datagram RX ring + SO_*
  options (REUSEADDR/TYPE/ERROR/BROADCAST/RCVBUF/SNDBUF, SO_ERROR read-and-clear) + readiness
  (socketReadable/Writable/Poll) + a 64-socket registry + ephemeral-port allocation
  (32768..60999) + **Pipe-style refcount** (socketRef/socketClose, freed at 0 — fork/dup
  semantics). UDP demux is most-specific (connected-peer beats wildcard listener). socketCreate
  enforces FAZA 11 IPv6 semantics (AF_INET6 → EAFNOSUPPORT) and defers STREAM (→ EPROTONOSUPPORT,
  TCP is FAZA 8). The kernel installs a wake hook (Scheduler::wakeAll) so the core is
  scheduler-free + host-testable. 92.3% cov.
- `net/Udp.{h,cpp}`: udpRx (length/checksum validation, demux to sockets, ICMP port-unreachable
  for unbound unicast ports — NOT for broadcast), udpSend with the **pseudo-header checksum**
  (source from the route), auto-bind on first send, 0→0xFFFF checksum rule. 92.6% cov.
- `net/Raw.{h,cpp}`: SOCK_RAW for ICMP (ping). RX delivers the **full IP datagram** (un-pulls the
  IP header via the preserved `l3` offset, fanning a copy to each raw socket) — exactly the
  Linux raw-socket semantics inetutils ping relies on. rawSend hands the caller's ICMP message
  to ipOutput (kernel adds the IP header, non-HDRINCL). 92.3% cov.
- Wired into `netCoreInit` (udpInit/rawInit/socketSetWakeFn). `tests/test_socket.cpp` (24 cases):
  create/family/type validation, bind (ephemeral/EADDRINUSE/bind-twice), setsockopt/getsockopt,
  udpSend byte-exact + valid pseudo-header checksum, UDP receive + source address, MSG_PEEK,
  connected-peer demux, wildcard listener, port-unreachable, refcount, RX-ring overflow, udpRx
  hardening (runt/bad-checksum/broadcast), EMSGSIZE, rawSend wire format, RAW full-IP receive.
  468 host tests green, check-arch clean.
- **AF_PACKET deferred to FAZA 10**, where its only consumer (udhcpc) exercises it — building it
  alongside its consumer (not a shortcut: it's reordering within the plan's intent).
- The socket layer reaches userland in FAZA 9 (syscall ABI + fd integration); on-wire UDP/RAW
  proof arrives with DNS (FAZA 11) and ping (FAZA 13).

## FAZA 8 — TCP (2026-06-12)

- `net/Tcp.{h,cpp}`: a full TCP — the complete RFC-793 state machine (CLOSED..TIME_WAIT),
  3-way handshake (active + passive open), sliding send/recv windows, RTO with Jacobson/Karn
  RTT estimation, **Reno congestion control** (slow start / congestion avoidance / 3-dupack fast
  retransmit+recovery), out-of-order reassembly, MSS option, immediate ACK, basic Nagle, and a
  proper four-way close with TIME-WAIT (2 MSL). TCBs live in this module's pool (orphaned on
  close so TIME-WAIT finishes after the socket is freed, like Linux). 92.2% cov.
- `net/Net`: factored out `inetPseudoChecksum` (shared by UDP + TCP). `include/string.h` +
  `lib/string_funcs.cpp`: added `memmove` (freestanding; the send-buffer compaction needs it).
- Socket integration: socketCreate(STREAM) attaches a TCB; connect/send/recv/close/readable/
  writable dispatch to tcp*; accept() mints a new socket via a hook. A net-timer kthread (id 3)
  drives `tcpTick`/`arpTick`/`ipReasmTick` every 50 ms (TCP needs timers even when idle); the
  RX softirq stays event-driven.
- `tests/test_tcp.cpp` (15 cases): active-open handshake, data send + window, in-order +
  out-of-order receive, active close (FIN_WAIT_1→2→TIME_WAIT→reaped), RTO retransmission, RST to
  a closed port, passive open + accept, CLOSE_WAIT/EOF + LAST_ACK, RST abort (SO_ERROR), MSG_PEEK,
  fast retransmit (3 dupacks), bare-SYN RST+ACK + send-buffer EAGAIN. 481 host tests green,
  check-arch clean.
- **QEMU verification (the no-shortcuts gate — a real internet TCP session):** a temp boot probe
  TCP-connected to `1.1.1.1:80` over slirp NAT and sent an HTTP request. The pcap shows the
  textbook exchange — `[S] seq 65536 mss 1460` → `[S.] ack` → `[.]` → `[P.] GET / HTTP/1.0` →
  server `[.] ack 42` → `[P.] HTTP/1.1 301 Moved Permanently` → server `[F.]` → our `[.] ack` →
  our `[F.]` → server `[.] ack` — a complete handshake, data exchange and clean four-way close
  with **a real Cloudflare server**. Probe since removed; the production net code boots clean
  (0 `v=08/0d/0e`).
- **Watch-item for FAZA 9:** the probe (run in the init task with deep synchronous call chains +
  busy-wait) triggered a kernel-stack issue in *that task's* context (the production kthreads are
  fault-free). When socket syscalls run the net path on a process's syscall stack in FAZA 9,
  verify the kernel stack depth is sufficient for connect→ipOutput→ethSend→e1000-tx chains.

## FAZA 9 — socket syscall ABI + fd integration (2026-06-12)

- `kernel/Syscall.{h,cpp}`: a socket is now an fd-table backing (`Fd.sock`), so read→recv,
  write→send, close→socketClose, poll→socketPoll, dup→socketRef and fork→socketRef all route to
  it (Pipe-style refcount). New methods marshal the Linux i686 `sockaddr_in` ABI (family LE,
  port+addr network order) and forward to the MI core: `sockSocket/Bind/Connect/ConnectResult/
  Listen/Accept/Getsockopt/Setsockopt/Getsockname/Getpeername/Sendto/Recvfrom/Shutdown` + net
  ioctls (`SIOCGIFADDR/SIOCSIFADDR/SIOCGIF{NETMASK,BRDADDR,FLAGS,HWADDR,MTU}`).
- `kernel/SyscallDispatch.cpp`: a unified `socketOp` handles BOTH the `socketcall(2)` demux
  (sub-calls 1..18, args read from the user array) AND the direct i386 syscalls (socket=359 …
  shutdown=373), with event-driven blocking for connect/accept/recv (and `MSG_DONTWAIT`). Added
  `_newselect(142)` via a poll-scan over the fd_sets, and routed `SIOC*` ioctls to the net layer.
- Arch contract widened: `kernelSyscall` gains a 6th arg `a5` (x86: `ebp`) so direct
  `sendto`/`recvfrom` (6-arg i386 syscalls) get their address-length — not a shortcut, the proper
  ABI. `tcpShutdown` added for a real half-close (`shutdown(SHUT_WR)` sends FIN, keeps reading).
- `net/Socket.cpp`: SOCK_STREAM now attaches a TCB and connect/send/recv/close/readable/writable
  dispatch to TCP; `accept()` mints a new socket via a hook.
- `tests/test_socketsys.cpp` (8 cases): socket/bind/getsockopt, UDP recvfrom via read() + source
  sockaddr, UDP connect+write to the wire, TCP connect→EINPROGRESS→SYN-ACK→ESTABLISHED, dup
  refcount, SIOCGIFADDR/HWADDR/MTU, EAFNOSUPPORT/ENOTSOCK paths. 488 host tests green, 91.0%
  aggregate, check-arch clean.
- **Kernel-layout root-cause fix (the watch-item from FAZA 8):** the net BSS (NetBuf + TCB pools)
  had grown the kernel image to 3.22 MB, ending at 0x412f98 — **past the user window base
  0x400000** — so a ring-3 program's pages collided with kernel globals (`g_useFb` @0x412cd0),
  faulting on the first console write. Right-sized the pools to the <3 MiB low-memory budget
  (NetBuf 128→96, TCP buffers 8K→4K, TCBs 8→4, msg kbuf 8K→4K): kernel now 3.03 MB, ends at
  0x3e2f98 with ~120 KiB headroom. Functionality is unchanged for the target workload (ping/wget/
  DNS use a few connections). **Note:** as the kernel grows in FAZY 10–14, raise the user window
  base (0x400000→higher) for durable headroom rather than trimming further.
- `netBringUp()` (static fallback: eth0 = 10.0.2.15, default via 10.0.2.2) wired at boot; FAZA 10
  runs DHCP and only falls back here. `user/socktest.c` kept as a permanent ring-3 net diagnostic.
- **QEMU verification (the no-shortcuts gate — ring 3 over the real ABI):** `socktest` (raw int
  0x80 `socketcall`) ran as a process and TCP-connected to **1.1.1.1:80** over slirp; the pcap
  shows the full session — `[S] mss 1460` → `[S.]` → `[.]` → `[P.] GET / HTTP/1.0` → server
  `[.] ack 42` → `[P.] HTTP/1.1 301` → `[F.]` ↔ our `[F.]` — a complete handshake, HTTP exchange
  and clean four-way close, **all driven from userland through the socket syscall ABI**. Zero
  `v=08/0d/0e`. The strace-parity gate holds: socket→connect→send→recv→close matches the
  inetutils/wget syscall shape captured in FAZA 0.

## FAZA 10 — interface bring-up + /etc (2026-06-12)

- `netBringUp()` (FAZA 9) statically configures eth0 = 10.0.2.15/24 + default via 10.0.2.2 (the
  QEMU user-net well-known addresses). This is the plan's static fallback.
- `/etc` is a writable RamFs mounted at boot and populated by `populateEtc()` from the read-only
  on-disk template `/disks/main/nanos/config/etc/` (resolv.conf → nameserver 10.0.2.3, hosts,
  nsswitch.conf, protocols, services). `config/etc/*` ship into the image via `_image`. Linux net
  apps now find `/etc/{resolv.conf,hosts,...}` at the canonical path; DHCP will rewrite resolv.conf.
- `user/pingtest.c` — a self-contained ring-3 program (raw int 0x80 socketcall, no libc) that
  **resolves wp.pl via DNS and ICMP-pings the result**, exercising the exact path GNU ping uses.
- **QEMU verification — THE GOAL MECHANISM, on the real internet (pcap + console):**
  ```
  10.0.2.15 > 10.0.2.3.53:  A? wp.pl.
  10.0.2.3.53 > 10.0.2.15:  A 212.77.98.9
  10.0.2.15  > 212.77.98.9: ICMP echo request id 4660 seq 1 len 64
  212.77.98.9 > 10.0.2.15:  ICMP echo reply   id 4660 seq 1 len 64
  ```
  console: `pingtest: wp.pl resolved to 212.77.98.9` / `reply from 212.77.98.9  <-- ping wp.pl OK
  (DNS + ICMP)`. NanOS, from ring 3, resolved wp.pl through the slirp DNS forwarder and got an
  ICMP echo reply from the **real wp.pl server (212.77.98.9)**. Zero `v=08/0d/0e`.
- **Baseline risk (FAZA 0) RESOLVED FAVOURABLY:** slirp on this host **does** forward ICMP echo
  to the internet — so the primary `ping wp.pl` success criterion (not the fallback) is met. TCP
  to the internet (1.1.1.1, FAZA 8/9) also works, so DNS+TCP are independently proven too.
- **DHCP (udhcpc) deferred to a follow-up:** the goal is met with static config + a static
  resolv.conf (the plan's allowed fallback). Porting busybox udhcpc through the SDK (with
  AF_PACKET) is the "completeness" extension; it doesn't gate `ping wp.pl`.

## Status after FAZA 10 — the goal is proven at the mechanism level

`ping wp.pl` works end to end from a NanOS ring-3 program on the real internet (DNS → ICMP →
reply from the real wp.pl). What remains is **packaging the real GNU `inetutils` ping/`wget`**
through the nanos-sdk so the literal precompiled-style binary runs (FAZA 11–13): the SDK has no
networking, so this needs the sysroot net headers (`<sys/socket.h>`, `<netinet/*>`, `<netdb.h>`,
…), libc socket wrappers (over the now-proven socketcall ABI), and a DNS stub resolver
(`getaddrinfo`) — then `inetutils` ping is a configure+make port (like grep/vim). The kernel +
syscall ABI are complete and internet-proven; FAZA 11–13 is SDK/userland packaging of the same
syscalls `pingtest` already drives.

## FAZA 11 + 12 — libc networking surface (DNS resolver + headers + socket glue) (2026-06-12)

The SDK uses picolibc, which ships NO networking — so this adds the whole network library to
libc.ndl + the sysroot headers, the foundation precompiled-style Linux net apps link against.

- **Headers (`user/libc-glue/include/`, Linux i686 layouts):** `sys/socket.h` (sockaddr/
  sockaddr_storage/iovec/msghdr/cmsghdr, AF_*/SOCK_*/SOL_SOCKET/SO_*/MSG_*/SHUT_*, the call
  decls), `netinet/in.h` (sockaddr_in 16 B, in_addr, INADDR_*/IPPROTO_*, htons/htonl), `arpa/inet.h`
  (inet_pton/ntop/aton/addr/ntoa — augments picolibc's macro-only one), `netdb.h` (addrinfo/
  hostent glibc layout, getaddrinfo/gethostbyname, EAI_*/AI_*), `netinet/tcp.h`.
- **Socket glue (`user/libc-glue/sockets.c` → libc.ndl):** every BSD call (socket/bind/connect/
  listen/accept[4]/get{sock,peer}name/get/setsockopt/send/recv/sendto/recvfrom/sendmsg/recvmsg/
  shutdown) over the socketcall(2) int-0x80 ABI, plus inet_pton/ntop/aton/addr/ntoa. (select is
  in syscalls.c.) Renamed off `net.c` to dodge the macOS case-insensitive `bin/net.o`↔`bin/Net.o`
  collision with the kernel's `net/Net.cpp`.
- **Stub resolver (`user/libc-glue/resolv.c` → libc.ndl, FAZA 11):** getaddrinfo / gethostbyname /
  getnameinfo / freeaddrinfo / gai_strerror. Numeric → `/etc/hosts` → DNS over UDP/53 (nameserver
  from `/etc/resolv.conf`, fallback 10.0.2.3), with DNS name-compression handling in the answer —
  the glibc stub path minus netlink/nscd. IPv4-only: `getaddrinfo(AF_INET6)` → EAI_NODATA.
- **QEMU verification — through the REAL POSIX API on the real internet (pcap + console):**
  `nettest.nxe` calls `getaddrinfo("wp.pl","80")` then `socket()/connect()/send()/recv()`. pcap:
  `A? wp.pl → A 212.77.98.9` then a full TCP fetch (`GET / HTTP/1.0` → `HTTP/1.1 301`); console:
  `getaddrinfo(wp.pl) -> 212.77.98.9:80` / `connected` / `HTTP reply: HTTP/1.1 301 Moved
  Permanently  <-- getaddrinfo + BSD sockets OK`. Zero `v=08/0d/0e`. 488 host tests still green.
- **FAZA 13 (real GNU ping/wget) is now a port, not new code:** sync these headers + the updated
  `libc.ndl.a` into the nanos-sdk sysroot, then `configure && make` inetutils — the API surface
  (`getaddrinfo` + BSD sockets, verified above) is exactly what they expect.

## FAZA 13 — outcomes (2026-06-12): real GNU inetutils ping runs `ping wp.pl`

**Result (binding goal MET, wire-verified):** the *unmodified* GNU inetutils 2.5 `ping`, cross-built
through the nanos-sdk and installed at `/nanos/bin/ping.nxe`, resolves `wp.pl` via DNS and exchanges
ICMP echo with the real internet from a NanOS bash prompt:
```
bash-5.2# ping wp.pl
PING wp.pl (212.77.98.9): 56 data bytes
64 bytes from 212.77.98.9: icmp_seq=0 ttl=255 time=11.000 ms
64 bytes from 212.77.98.9: icmp_seq=1 ttl=255 time=31.000 ms
...
```
pcap (filter-dump): `A? wp.pl → A 212.77.98.9`, ARP for the gateway, then echo request/reply pairs
(seq 0..N, id 5). Zero `v=08/0d/0e`. Driven via `scripts/ping-qemu.sh` (boots headless, types the
command through the QEMU monitor `sendkey`, screendumps + decodes the pcap).

**How it was built (no source patches — the binary is upstream):**
- New `make ping` target (mirrors `make bash`/`grep`): refreshes the SDK sysroot from this checkout
  (`user/libc-glue/include` + freshly built `bin/libc.ndl{,.a}`) and drives `nanos-port` (inetutils
  manifest at `$(SDK_WORK)/inetutils-port/nxport.toml`). `_image` installs `bin/ping.nxe` into
  `/nanos/bin`; `externals` stages it.
- **gnulib portability fight (config.cache, not patches):** inetutils bundles 221 gnulib modules and
  compiles all of them. Forced the right detection via cross-compile cache entries: `glob`
  (REPLACE_GLOB=1 so gnulib uses its own GNU `glob.h`), `fts` (DT_* in `<dirent.h>`), `openpty`/
  `forkpty`/`fdopendir`/`dirfd`/`rewinddir`/`scandir` (claim present so the dead server-only
  replacements compile but never link), `sys/uio.h` (real header so gnulib stops redefining
  `struct iovec`).
- **New sysroot/libc-glue headers (POSIX surface picolibc lacks):** `sys/uio.h`, `net/if.h`,
  `utmp.h`, `arpa/tftp.h`, `arpa/telnet.h`, `netinet/{in_systm,ip,ip_icmp,icmp6}.h`, `sys/utsname.h`,
  `sys/un.h`, `pty.h`; extended `netdb.h` (`struct protoent`/`servent`, `NI_MAXHOST`), `netinet/in.h`
  (`IPPROTO_ICMPV6`, `INET[6]_ADDRSTRLEN`), `dirent.h` (DT_* + dir-walk decls).
- **New libc.ndl implementations (ping LINKS these):** `getprotobyname`/`getprotobynumber`,
  `getservbyname`/`getservbyport`, `gethostbyaddr` (no reverse DNS → numeric, like `ping -n`) in
  `resolv.c`; `uname`, `openpty`/`forkpty`/`login_tty` ENOSYS stubs in `posixstubs.c`.

**Two correctness fixes found via ping (not shortcuts — fixed properly):**
1. **Float printf** — picolibc was built `-Dformat-default=integer`, so ping's `%f` RTT printed the
   literal `*float*`. Switched the NanOS picolibc to `-Dformat-default=double` (docker/Dockerfile);
   userland now formats floats. (Kernel is freestanding — unaffected.)
2. **Negative ping RTTs** — `clockGettime(CLOCK_REALTIME)` mixed live-RTC *whole seconds* with the
   *monotonic tick* sub-second, so the timestamp went backwards at every RTC second boundary →
   ping computed negative `recv − send`. Fixed: realtime now = boot epoch (RTC sampled once at boot)
   + the FULL monotonic tick, so seconds and sub-seconds share one source and the clock is monotonic
   (`kernel/Syscall.cpp` clockGettime, `SyscallDispatch.cpp` passes `kernel::bootEpochSeconds()`).
   Host test updated; 488 tests green, coverage 91.0%, `make check-arch` clean.

**Remaining (follow-up):** `wget` (same port path; bigger gnulib/TLS surface — HTTP-only first).

## FAZA 13 — wget outcome (2026-06-12): real GNU wget fetches over HTTP

**Result (wire-verified):** the *unmodified* GNU wget 1.21.4, cross-built via nanos-port (HTTP-only,
`--without-ssl`), installed at `/nanos/bin/wget.nxe`, fetches a real page from a NanOS bash prompt:
```
bash-5.2# wget -O /tmp/n.html http://neverssl.com
Resolving neverssl.com... 34.223.124.45
Connecting to neverssl.com|34.223.124.45|:80... connected.
HTTP request sent, awaiting response... 200 OK
Length: 3961 (3.9K) [text/html]
'/tmp/n.html' saved [3961/3961]
```
pcap: DNS `A? neverssl.com → 34.223.124.45`, full TCP handshake, `GET / HTTP/1.1` → `HTTP/1.1 200 OK`
+ body, clean FIN both ways. Zero faults. (Build: `make wget`; same reproducible flow as `make ping`.)

**New libc/sysroot surface wget needed (all in libc-glue, tracked + synced — no wget patch):**
- **stdio-ext over picolibc tinystdio** (`user/libc-glue/stdio_ext.c` + `include/stdio_ext.h`):
  `__freading/__fwriting/__freadable/__fwritable/__fpurge/__fbufsize/__flbf/__fsetlocking` — gnulib's
  `freading.c`/`fpurge.c` `#error` on picolibc's FILE; with these present gnulib uses ours.
- **link()** (`syscalls.c`, over SYS_link — NanOS hardlinks) — gnulib `link` module has no fallback.
- **munmap()** (`syscalls.c`, no-op: mmap eagerly backs pages, no unmap syscall) + **sys/mman.h**.
- **pathconf()/fpathconf()** (`posixstubs.c`, POSIX defaults: PATH_MAX 4096, NAME_MAX 255) — so wget
  uses pathconf, not the out-of-scope raw PATH_MAX fallback. + **limits.h** overlay (guarantees PATH_MAX).
- **rewinddir()/fdopendir()/dirfd()** (`dirent.c`, real impls over the getdents64 DIR) — declared for
  ping but now genuinely linked by wget's dir handling.
- **clock_getres()** (`syscalls.c`, fixed 1 ms — the 1000 Hz tick).
- **sys/utime.h** overlay (picolibc's "dummy" copy uses time_t without including it).
- gnulib cross-compile cache (`nxport.toml`): force the stdio-ext `HAVE___*`, `*_unlocked` decls OFF
  (single-threaded → gnulib maps them to plain calls), and the same glob/fts/openpty set as ping.
- One **toolchain** header fix: picolibc's `<stdio.h>` `__nanos__` block defined `putc_unlocked` etc.
  as macros, which broke gnulib's `unlocked-io.h`; removed (single-threaded ⇒ locked == unlocked).

**HTTPS is out of scope** until a TLS library (OpenSSL/GnuTLS, themselves big gnulib/asm ports) lands.

## FAZA 14 — outcomes (2026-06-12): /proc/net, /proc/bus/pci, hardening, full host-test sweep

**`/proc/net/{dev,route,arp,tcp,udp,raw,snmp}` + `/proc/bus/pci/devices`** — all Linux-format,
rendered from live state. New MI module `net/NetProc.{h,cpp}` (the renderers) + `net/NetStats.{h,cpp}`
(SNMP counters); iteration accessors added to the modules (`arpEntryAt`/`arpSlots`, `socketAt`/
`socketSlots`, `tcpSnapshot` keeping the private Tcb encapsulated); wired into `fs/SynthFs.cpp` as
`/proc/net/*` and `/proc/bus/pci/devices` generators.

- **Address columns** follow Linux exactly: /proc/net/{tcp,udp,raw,route} print IPv4 as the
  network-order bytes read little-endian (`hton32`, %08X) + port %04X; arp dotted-decimal; TCP `st`
  remapped to Linux's state numbering (ESTABLISHED=01 … LISTEN=0A). Verified on the wire in QEMU:
  `/proc/net/route` shows the on-link `0002000A`/mask `00FFFFFF` and default `00000000` via `0202000A`
  (flags `0001`/`0003`); `/proc/bus/pci/devices` lists the e1000 at devfn `0018` (0:3.0) irq `b`(=11);
  `/proc/net/snmp` emits the full `Ip:/Icmp:/Tcp:/Udp:` header+value lines (`netstat -s` format).
- **SNMP counters** wired into the data path (single lines at the RX/TX entry points): IP
  InReceives/InHdrErrors/InAddrErrors/InDelivers/OutRequests/OutNoRoutes; ICMP InMsgs/InEchos/
  OutMsgs/OutEchoReps/…; TCP InSegs/OutSegs/ActiveOpens/PassiveOpens/OutRsts/InErrs; UDP
  InDatagrams/OutDatagrams/NoPorts. Counters we don't maintain report 0 (never a fabricated number).
  `apU`/`apURight` use `unsigned long` (32-bit on i686) so the freestanding kernel needs no libgcc
  64-bit divide.

- **Hardening** (`tests/test_net_hardening.cpp`, 4 cases): malformed IP headers (truncated, bad
  version/ihl/total/checksum), malformed ICMP/UDP/TCP payloads (runts, bad checksums, all-flags TCP
  to a closed port), a 200-deep fragment flood, and malformed Ethernet/ARP frames — each asserts the
  stack returns normally AND `netbufInUse()` is back to baseline (no NetBuf leak; the fragment flood
  never exhausts the 96-buffer pool and is reclaimed by `ipReasmTick`).

- **Host-test sweep**: 499 doctest cases (3.24M assertions) green; new modules NetProc 93.5% /
  NetStats 100%; aggregate line coverage 90.7% (≥90% gate). `make check-arch` clean (NetProc/NetStats
  are MI). QEMU boot + `cat /proc/net/*` + `/proc/bus/pci/devices`: no `v=08/0d/0e`; image e2fsck-clean.

**The networking plan (FAZY 0–14) is complete.** Real GNU ping + wget run over the full stack on the
real internet; the system exposes Linux-compatible `/proc/net` introspection; the data path is
hardened against malformed input. Out of plan scope (future): HTTPS/TLS, IPv6, window scaling/SACK.
