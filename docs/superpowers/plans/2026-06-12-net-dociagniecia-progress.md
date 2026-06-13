# Networking follow-up — progress log

Companion to `2026-06-12-net-dociagniecia.md`. One row per phase. Gates are the no-shortcuts bars
from §2 (host-tests ≥90%, pcap field-compare with a random-field mask, `check-arch` clean, QEMU no
`v=08/0d/0e`, e2fsck-clean). **All phases DONE.**

| Faza | Status | host-test | wire / QEMU | commit(s) |
|---|---|---|---|---|
| A — docs corrections | **DONE** | n/a | n/a | — |
| B — memory budget (user window 0x800000, pools 128/16/8192) | **DONE** | size-only, suite green | boot+ping+wget on new base | — |
| C — ICMP errors → sockets | **DONE** | test_icmp_err 100% | UDP→ICMP port-unreach → ECONNREFUSED | — |
| D — TCP options+timers (wscale/TS/SACK/delACK/persist/keepalive + SYN rtx) | **DONE** | test_tcp(_opts) | SYN `[mss,sackOK,TS,nop,wscale]` like Linux | — |
| E — resolver + /etc (services/protocols, TCP/53, search/ndots, multi-ns, PTR) | **DONE** | test_resolv | dig-faithful PTR; reads /etc files | — |
| F — AF_PACKET + real DHCP (busybox udhcpc) | **DONE** | test_packet | DISCOVER→OFFER→REQUEST→ACK, eth0=10.0.2.15 by DHCP | — |
| G — AF_UNIX + socketpair + /proc/net/unix | **DONE** | test_unix/socketsys/netproc | unixtest ALL PASS; `srwx` node in /tmp | — |
| H — services: inetd + telnetd + darkhttpd, started at boot | **DONE** | 546 tests, 91.1% | host: curl :5555, telnet :2323→bash, daytime/echo; 40/40 soak; 0 faults | 039e1b1 aab8c5f bddb900 1fb6ac3 98c57be 8598e14 4162f1a 19e5d3b 15c4b68 |
| I — nettools: ifconfig + traceroute + telnet client | **DONE** | 546 tests, 91.1% | ifconfig eth0/lo; traceroute hop+ICMP-TE; telnet 127.0.0.1→bash | 2772d88 323a7d4 |
| J — sweep: hardening, /proc/net LISTEN, docs, e2fsck | **DONE** | 546 tests, 91.1% | /proc/net/tcp all `0A`; image e2fsck-clean; 0 faults | (this set) |

## FAZA H — outcomes (2026-06-13)

Three inetutils/darkhttpd servers, all unmodified upstream (config + sysroot headers only):

- **inetd** (`make inetd`): internal echo/discard/daytime/chargen + launches telnet→telnetd.
  Needed new syscalls **pause/sigsuspend** + libc **wait/execv**. Config `/etc/inetd.conf`.
- **telnetd**: remote bash login over the kernel pty. Needed **TIOCPKT packet mode** in the pty
  (telnetd treats the first master-read byte as a status preamble) and **login_tty** acquiring the
  controlling terminal + foreground pgrp (else bash SIGTTIN-stops). New `nanologin` login shim.
  Sysroot headers completed: telnet.h SLC/LINEMODE, syslog facilities, ioctl FIONBIO/TIOCPKT.
- **darkhttpd** (`make httpd`): HTTP/1.1 on :80 serving `/apps/www`. Needed libc **pread/pwrite**
  + chroot/getrusage stubs, and two real TCP fixes: **honor TCP_NODELAY** (was a no-op) and
  **flush the send buffer on close** (a body written just before close raced the FIN and was lost).
- **boot**: `init` starts inetd + darkhttpd after DHCP (they self-daemonize). The system is
  reachable from the macOS host straight off a boot. A 40-GET soak exposed **TCB exhaustion** under
  active-close TIME_WAIT; fixed by **recycling the oldest TIME_WAIT** under pressure (tcp_tw_reuse).

Verified from the host (scripts/{services,telnet,httpd,boot-services,svc-loop}-qemu.sh): bash login
over telnet, 200+page over HTTP (4/4 concurrent), echo/daytime, two telnet sessions reuse the one
pty pair (no leak), zero CPU faults.

## FAZA I — outcomes (2026-06-13)

Three diagnostic clients from the same inetutils build (`make inetd` stages them):

- **ifconfig**: lists eth0 (live DHCP 10.0.2.15) + lo. Needed libc **if_nametoindex/indextoname/
  nameindex/freenameindex** (ifname.c) over SIOCGIFINDEX — no SIOCGIFCONF needed.
- **traceroute**: needed **setsockopt(IP_TTL)** (per-socket TTL plumbed through ipOutput) — which
  also exercises FAZA C (ICMP time-exceeded → socket). `traceroute 10.0.2.2` → gateway hop on the
  pcap (UDP TTL 1..n + ICMP time-exceeded).
- **telnet client**: needed a **loopback route 127/8 → lo** (self-connect was leaking to the
  gateway) + libc execl/TCFLSH. `telnet 127.0.0.1 23` → our own telnetd → bash, fully guest-local.

**Known port quirk:** inetutils argp **short** options break against picolibc's getopt even with
REPLACE_GETOPT forced; the tools are driven with long options/positionals. A proper libc getopt is
tracked separately (it does not block the diagnostics).

## FAZA J — outcomes (2026-06-13)

- **Hardening** (test_net_hardening.cpp): malicious/truncated ICMP errors are demuxed-and-dropped
  (no mis-delivery, no leak); a 100-SYN flood to a listener never exhausts the TCB table or leaks.
- **/proc/net/tcp**: all six listeners (echo/discard/daytime/chargen/telnet/http) render state
  `0A` (LISTEN) in Linux format.
- **docs/networking.md**: §5 (DHCP), §9 (servers+tools), §10 (difference table reflects reality),
  §13 (out-of-scope trimmed) rewritten.
- **e2fsck-clean**: `make image` now ends with `e2fsck -fy` to reconcile bitmaps after the debugfs
  writes; the image is e2fsck-clean. Full QEMU scenario (boot→DHCP→services from host→traceroute→
  telnet→/proc/net) zero faults. 546 host tests, 91.1% aggregate, `check-arch` clean.

## Acceptance (plan §"Kryteria akceptacji całości")

1. **From macOS host:** `telnet localhost 2323` → bash login; `curl http://localhost:5555/` → 200
   from darkhttpd. ✓
2. **DHCP, not static:** boot configures eth0 via real busybox-udhcpc; static = loud fallback. ✓
3. **Our SYN = Linux SYN** `[mss,sackOK,TS,nop,wscale]`; delayed ACK, persist, keepalive present. ✓
4. **`/etc` is truth:** services/protocols/resolv.conf(search,ndots,multi-ns)/PTR read from files;
   traceroute works (ICMP errors reach sockets). ✓
5. **AF_UNIX + socketpair + AF_PACKET** real; `/proc/net/unix` Linux format. ✓
6. Ports unmodified (manifests/cache only); `make test` ≥90%; `check-arch` clean; QEMU no
   `v=08/0d/0e`; commits with no AI mention. ✓
