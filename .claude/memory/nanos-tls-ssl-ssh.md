---
name: nanos-tls-ssl-ssh
description: OpenSSL/TLS/SSH on NanOS — ALL 5 phases done (CSPRNG, OpenSSL, TLS client+server, SSH server via Dropbear); one follow-up: Dropbear session teardown lingers
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

Plan `docs/superpowers/plans/2026-06-13-tls-ssl-ssh-openssl.md` ("najpierw entropia, potem TLS").
Progress as of 2026-06-13:

- **FAZA 0 — kernel CSPRNG (DONE):** ChaCha20 fast-key-erasure in `kernel/Csprng.{h,cpp}` (MI,
  host-tested, RFC 8439 vector), seeded from RDRAND (`arch/x86/cpu/random_x86.cpp` via
  `<arch/random.h>`) + RDTSC jitter + RTC. Replaced the two FIXED-seed RNGs. Backs
  `/dev/random`+`/dev/urandom`+`SYS_getrandom`(355)+`getentropy`. `make run/run-net` use `-cpu
  Nehalem` for RDRAND. `randhex.nxe` proves two boots differ.
- **FAZA 1 — OpenSSL 3.0.15 port (DONE):** external port `~/Projects/nanos-sdk-work/openssl-port/`
  (`make openssl`), no source patches. Configure: generic `gcc` target, `no-asm/no-threads/no-shared`,
  `-fno-pie -no-pie` via **env CFLAGS/LDFLAGS** (OpenSSL's option parser eats a bare `-no-pie`),
  `--openssldir=/disks/main/nanos/ssl`, seed=devrandom. **Real kernel bug fixed:** the per-process
  user window was ONE 4 MiB PDE (image+stack); openssl (~3.7 MiB) overran into the stack region so
  `archLoadUser`'s zeroed stack frames wiped IAT slots → jmp to 0. Widened to **8 MiB / 2 PDEs**
  (`NX_USER_*` in `mmu_x86.cpp`, `AddressSpace::dropPde`, STAGE_CAP=8MiB, stack at 0xF80000). Also
  added `struct linger` + `symlink()` wrapper. Smoke: version=3.0.15, rand differs per call,
  dgst==host sum, genrsa 2048 works.
- **FAZA 2.1-2.2 — verified TLS client (DONE):** Mozilla CA bundle `disk-content/ssl/cert.pem` →
  `/nanos/ssl/cert.pem`. `openssl s_client -connect example.com:443` → TLS 1.3, X25519,
  **Verification: OK**. Needed kernel fix: `ioctl(FIONBIO)` → O_NONBLOCK (OpenSSL BIO; was EPERM).
- **FAZA 2 — TLS client (DONE):** wget rebuilt `--with-ssl=openssl` → `wget https://example.com`
  = 200 + saved. Needed: drop `no-deprecated` from openssl (wget probes SSL_library_init); add
  `<sys/random.h>` (gnulib getrandom). FIONBIO→O_NONBLOCK kernel fix. 2.3 satisfied by real-CDN
  interop (unmodified OpenSSL = same wire as Linux).
- **FAZA 3 — TLS server (DONE):** ship default openssl.cnf to OPENSSLDIR; guest `openssl req -x509`
  self-signs on-device (genrsa on CSPRNG) + `s_server -WWW`; host `curl -k https://localhost:5443/`
  → page. hostfwd 5443->5443 (+2222->22) in NIC_NET.
- **FAZA 4 — SSH server (Dropbear) DONE:** `~/Projects/nanos-sdk-work/dropbear-port/`
  (`make dropbear`), Dropbear 2022.83. From host: `ssh -p 2222 root@localhost` (password `nanos`)
  OR `ssh -i key -p 2222 root@localhost` (pubkey) → bash over the kernel PTY (SSH-2 + ed25519
  hostkey). **Password auth** via NanOS `crypt(3)` (SHA-512 $6$ in `user/libc-glue/crypt.c`,
  byte-exact vs `openssl passwd -6`; `test_crypt.cpp`); root hash in `config/passwd`.
  Four fixes: **non-PIC** (CFLAGS=-fno-pie -fno-PIC, LDFLAGS=-no-pie) — default gcc PIC routes data
  symbols (stderr) via GOT, which mknx does NOT import → stderr=0 → fprintf faults; force-link all
  3 stdio streams in libc.ndl (was stdin only); `initgroups()` stub; `/etc/shells` (config/etc +
  populateEtc + image) so getusershell() accepts root's shell; root home → /disks/main/root.
  **Minor known wart:** non-interactive `ssh host cmd` lingers after the command (pty/channel EOF
  teardown); output returns fine.
- **FAZA 5 — client + sweep (DONE):** dbclient runs; /proc/net/tcp shows the sshd listener (:0016
  state 0A); image e2fsck-clean; `docs/crypto.md` documents the whole stack; progress log written.
- **512 MiB RAM:** the memory map is now RAM-size-agnostic — virtual windows lifted to ≥1 GiB,
  kernel heap scales with RAM. `make run` uses -m 512 (KHeapTotal ~128 MiB, ~368 MiB frame pool).
- **pty master EOF on slave close** added (CharDevice open/close hooks + Vfs deviceOpen/Close +
  fd-layer refcount) — correct tty semantics.

**Dropbear rebuilt for x86_64 with the signal-number fix (2026-06-19):** `make ARCH=x86_64 dropbear`
builds clean (the earlier "configure exit 77 / defaults to i686-nanos" was STALE STATE — a leftover
config.cache / a run without ARCH=x86_64 — NOT a real port defect). The build injects the corrected
glue `sys/signal.h` into the sysroot, so the binary bakes in **Linux SIGCHLD=17** (`__nanos__`
predefined, verified at symbol level). See [[nanos-bash-port]]/[[nanos-multiprocessing-roadmap]]
for the signal ABI. The fix itself (userland was at picolibc/BSD SIGCHLD=20, kernel posts Linux=17,
so dropbear's reaper never fired → zombies) is committed (1f14284) and PROVEN by `user/smoke/sigreap.c`
(dropbear's exact `sigaction(SA_NOCLDSTOP)`+`waitpid(WNOHANG)` pattern: FAIL-ZOMBIES-LEAK → OK-HANDLER-REAPS).
SSH verified working end-to-end on x86_64 (pubkey auth succeeds, remote `echo` returns over the session).

**x86_64 had NO involuntary preemption — FIXED 2026-06-19 (commit 75177a1).** `arch/x86_64/cpu/irq64.S`
returned to ring 3 with a bare `iretq`, never calling `schedPreempt` (a Plan 4 TODO left the hook
unwired); the x86_64 scheduler was effectively cooperative. A CPU-/syscall-bound user task that never
voluntarily yielded (e.g. dropbear busy-looping on a non-blocking `read`) starved EVERY other task —
`ksoftirqd-net` stopped processing packets, the console went dead, the whole system wedged. Fix: mirror
i686 `irq.S` — on the ring-3 return (saved CS at `[rsp+144]` with the 15 GP regs pushed) `call schedPreempt`.
The context-switch machinery (`switch64.S`/`schedPreempt` in `sched_x86_64.cpp`) already existed; only the
IRQ-return call site was missing. VERIFIED: console stays live during a busy session now (was dead before).
This is a foundational x86_64 robustness fix beyond SSH. See [[nanos-x64-migration]] / [[nanos-terminal-and-deferred-scheduler]].

**OPEN follow-up (dropbear teardown linger — NARROWED, no longer catastrophic):** non-interactive
`ssh host cmd` still doesn't cleanly return. Root cause pinned with the now-live console: the command's
shell (`bash.nxe`) becomes a **zombie** (`Z`) that the dropbear session-child never reaps; the session-child
spins on `read(fd=3)` — a **non-blocking PIPE** returning `-EAGAIN` forever (the command's stdout pipe never
hits EOF: a writer stays open even though `procExit` runs `closeAll()` and `fork` bumps pipe refcounts
correctly — likely dropbear's own unclosed write-end copy in the noptycommand path, or a NanOS close→pipe
writer-refcount edge). Before the preemption fix this spin wedged the whole box; now it only hogs ~½ CPU
(round-robin) and degrades new connections (banner timeout) but the system stays up + console responsive.
Exposed by the SIGCHLD=17 fix (before, the handler never fired so dropbear never entered the reap/close
teardown path). NEXT: read dropbear `svr-chansession.c` noptycommand pipe-close vs NanOS pipe EOF semantics.

Lessons: (1) any SDK-ported binary using libc DATA symbols (stderr/stdout) MUST be built non-PIC
(`-fno-pie -fno-PIC -no-pie`) — default PIC routes them via the GOT, which mknx does not import →
symbol resolves to 0 → fault. (2) dropbear needs /etc/shells (getusershell) + initgroups stub +
pubkey auth (no crypt). See [[nanos-networking-stack]] for the TCP base.

The whole `.nxe` toolchain runs large fixed-base ET_EXEC binaries; the 8 MiB user window is the
current ceiling. See [[nanos-sdk-and-vim-port]] for the SDK port flow, [[nanos-networking-stack]]
for the TCP/IP base.
