---
name: nanos-networking-stack
description: Full TCP/IP stack + real GNU ping & wget; how net ports are built and the correctness fixes they exposed
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NanOS has a complete, internet-proven networking stack (plan `docs/superpowers/plans/2026-06-12-networking.md`,
progress `…-networking-progress.md`). FAZY 0–14 ALL done: PCI → e1000 nkext → NetDevice/softirq → Ethernet/ARP →
IPv4/route → ICMP → sockets/UDP/RAW/TCP → Linux socketcall ABI → libc DNS resolver + BSD socket glue → **real
GNU inetutils ping + GNU wget** → **`/proc/net/{dev,route,arp,tcp,udp,raw,snmp}` + `/proc/bus/pci/devices`
(Linux format) + hardening**. All `net/` code is MI + host-tested (499 tests, ~91%); slirp on this macOS host
DOES forward ICMP to the real internet. The plan is COMPLETE; out of scope (future): HTTPS/TLS, IPv6, SACK.

**`ping wp.pl` works** with the *unmodified* inetutils 2.5 `ping` at `/nanos/bin/ping.nxe` (DNS → ICMP echo,
positive RTTs, no faults). **`wget http://neverssl.com` works** with *unmodified* GNU wget 1.21.4 at
`/nanos/bin/wget.nxe` (HTTP-only, `--without-ssl`; DNS → TCP → HTTP 200, file saved). Both built by `make
ping`/`make wget` (mirror `make bash`). Verify with `scripts/ping-qemu.sh "<cmd>"` (boots headless, types via
QEMU monitor `sendkey`, decodes the pcap). HTTPS is out of scope until a TLS lib is ported.

**Building external network ports** (the nanos-sdk path, see [[nanos-sdk-and-vim-port]]): `make ping` refreshes
the SDK sysroot from `user/libc-glue/include` + freshly built `bin/libc.ndl{,.a}`, then drives `nanos-port`
(manifest at `~/Projects/nanos-sdk-work/inetutils-port/nxport.toml`) in the `nanos-sdk-dev` container. The
toolchain lives at `~/Projects/nanos-sdk-work/toolchain` (NOT /opt in the image). gnulib-heavy autotools ports
need cross-compile `cache=[...]` entries to force correct feature detection (the conftest gcc wrapper makes
link probes honest); server-only modules (pty/utmp/glob/fts) are made to compile-but-not-link via cache + decl.
The SDK ships many POSIX headers picolibc lacks (`netinet/ip*.h`, `net/if.h`, `utmp.h`, `arpa/{telnet,tftp}.h`,
`sys/{uio,un,utsname}.h`, `pty.h`). `_image` installs `bin/ping.nxe` → `/nanos/bin`.

**Two correctness fixes ping exposed (both fixed properly, not worked around):**
1. picolibc was `-Dformat-default=integer` → `%f` printed literal `*float*`. Switched NanOS picolibc to
   `-Dformat-default=double` (`docker/Dockerfile`); userland now formats floats (kernel is freestanding, unaffected).
2. `clockGettime(CLOCK_REALTIME)` mixed live-RTC whole-seconds with the monotonic-tick sub-second → non-monotonic
   at second boundaries → **negative ping RTTs**. Fixed: realtime = boot epoch (RTC sampled once at boot, via
   `kernel::bootEpochSeconds()`) + the FULL monotonic tick. Both sec+nsec from one source. `kernel/Syscall.cpp`
   `clockGettime`, dispatch passes the boot epoch.

**wget needed extra libc/sysroot surface (all tracked in libc-glue, no wget patch):** stdio-ext over
picolibc tinystdio (`stdio_ext.c`: `__freading`/`__fwriting`/`__fpurge`/...), `link()`, `munmap()` (no-op) +
`sys/mman.h`, `pathconf()`/`fpathconf()` + `limits.h` overlay (PATH_MAX), `rewinddir`/`fdopendir`/`dirfd`
(`dirent.c`), `clock_getres()`, `sys/utime.h` overlay. One toolchain header fix: removed the
`putc_unlocked` etc. *macros* from picolibc `<stdio.h>`'s `__nanos__` block (broke gnulib unlocked-io.h).

Remaining: HTTPS (needs a TLS lib port) and FAZA 14 (`/proc/net` + hardening).
See [[nanos-multiprocessing-roadmap]] for the broader roadmap.

**Follow-up plan `docs/superpowers/plans/2026-06-12-net-dociagniecia.md`** (remove kludges, Linux-faithful, phases A–J):
A–F DONE. FAZA F = **AF_PACKET sockets + REAL DHCP** (`net/Packet.cpp`, busybox 1.36.1 udhcpc built from
`~/Projects/nanos-sdk-work/busybox-1.36.1/nanos-build.sh`). udhcpc acquires a lease over PF_PACKET/SOCK_DGRAM
(cooked: kernel strips/adds the Ethernet header); init runs it at boot (`user/init.c run_dhcp`, 10 s cap then
SIGKILL → kernel `netBringUp()` static is the *loud fallback*); action script is a compiled `.nxe`
(`user/dhcpcfg.c` → `/nanos/config/udhcpc.script`) that applies SIOCSIFADDR/NETMASK/ADDRT + writes resolv.conf.
Verified: pcap DISCOVER→OFFER→REQUEST→ACK, "lease of 10.0.2.15 obtained", zero faults.

**The DHCP-hang bug (subtle, reusable):** `recvmsg(2)` must report `msg_controllen = 0` when it emits no
ancillary data — Linux *overwrites* the caller's input length. We were leaving it at the caller's value.
busybox `d4_recv_raw_packet` passes an UNINITIALISED cmsg buffer with `msg_controllen=sizeof(buf)` and walks it
with CMSG_FIRSTHDR/CMSG_NXTHDR for PACKET_AUXDATA; a garbage `cmsg_len` spins CMSG_NXTHDR forever, so udhcpc
wedged right after a valid OFFER (no REQUEST, no resend, no fault). Fix in `kernel/SyscallDispatch.cpp`
SC_RECVMSG: set `m[5]=0` (msg_controllen) alongside `m[6]=0` (msg_flags). Two other udhcpc-path fixes: read(2)
on an AF_PACKET fd strips L2 (cooked); NULL-safe `fflush()` override in `user/libc-glue/stdio_ext.c` (picolibc
tinystdio derefs the stream with no guard but fflush(NULL) must flush all streams).

FAZA G = **AF_UNIX (local sockets) + real socketpair** DONE (`net/Unix.{h,cpp}`, MI, host-tested).
STREAM = a refcounted `UnixChannel` (two per-direction byte rings + both endpoints, woken via the existing
socketSetWakeFn hook); DGRAM reuses the Socket RXQ ring + socketDeliver (boundaries free). AF_UNIX lives in
the same g_socks table (socketCreate attaches per-socket UnixState behind `void* un`, like the `tcp` seam;
socketClose -> unixDetach wakes the peer with EOF + drops the channel ref). Naming is resolved by scanning
g_socks (the authority — NOT a VFS file). Syscall ABI: parseSockaddrUn/writeSockaddrUn + AF_UNIX branches in
sockBind/Connect/Listen/Accept/Sendto/Recvfrom; SC_SOCKETPAIR is real (was -ENOSYS). `/proc/net/unix` renders
the Linux columns. Verified: `unixtest.nxe` (raw int 0x80, /nanos/bin) prints ALL PASS in QEMU (socketpair
ping/pong + named connect/accept on /tmp/unixtest.sock), zero faults. **Deferred refinement:** a visible
S_IFSOCK node in the VFS (`ls /tmp`) — functionally redundant (MI registry is authority), and wiring
vfs->mknod into bind couples the layer + breaks the pure host tests; do it with RamFs S_IFSOCK + graceful
fallback when the path isn't on a writable FS. Next: H (services inetd/telnetd/httpd), I (nettools), J (sweep).
