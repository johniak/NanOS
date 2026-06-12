# Networking implementation — progress log

Companion to `2026-06-12-networking.md`. One row per phase; updated as each lands. The "gate"
columns are the no-shortcuts bars from §2 of the plan (host-tests ≥90%, pcap byte-compare,
strace parity, `check-arch` clean, QEMU no `v=08/0d/0e`).

| Faza | Status | host-test | pcap | strace | QEMU | commit |
|---|---|---|---|---|---|---|
| 0 — QEMU net + pcap harness + baseline + strace | **DONE** | n/a | harness ready | fixtures captured | n/a | — |
| 1 — PCI bus | todo | | | | | |
| 2 — e1000.nkext | todo | | | | | |
| 3 — NetDevice + bottom-half + lo | todo | | | | | |
| 4 — Ethernet + ARP | todo | | | | | |
| 5 — IPv4 | todo | | | | | |
| 6 — ICMP | todo | | | | | |
| 7 — sockets + UDP + RAW + AF_PACKET | todo | | | | | |
| 8 — TCP | todo | | | | | |
| 9 — socket syscall ABI | todo | | | | | |
| 10 — DHCP + iface bring-up | todo | | | | | |
| 11 — DNS resolver (libc) | todo | | | | | |
| 12 — SDK net headers | todo | | | | | |
| 13 — ping + wget | todo | | | | | |
| 14 — /proc/net + hardening | todo | | | | | |

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
