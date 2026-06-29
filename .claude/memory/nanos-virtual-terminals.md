---
name: nanos-virtual-terminals
description: "Linux-style virtual terminals (Ctrl+Alt+Fn) — 6 kernel text consoles + graphics F7, all phases done + QEMU-verified"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

Kernel virtual terminals — **MERGED into `main` (fast-forward, 19 commits, tip 2190004; not pushed
to origin/main yet)**; the `feat/virtual-terminals` branch is deleted. Spec
`docs/superpowers/specs/2026-06-24-virtual-terminals-design.md`, plan
`docs/superpowers/plans/2026-06-24-virtual-terminals.md`). All 6 phases done, `make smoke-vt` green
and wired into `verify64`; host tests 754 green.

**Model (Linux-faithful, in the kernel):** `kernel/vt/VtConsole` = one console (per-VT line
discipline + raw ring + wait queue + termios + fg pgrp + `FbConsole` with a `live` bit +
`KD`/`VT_SETMODE` state). `kernel/vt/VtManager` owns `vt[1..7]` + active index + the real
framebuffer, serializing ALL fb output + switching under one recursive IRQ lock (the old
`g_consoleLock` role). `drivers/VtTty` = `/dev/ttyN`; also `/dev/tty0` (active), `/dev/tty`
(controlling VT via `Process::cttyVt`/`TIOCSCTTY`), `/dev/console` (=tty1). tty1–6 = kernel fbcon
text consoles (init runs a getty/login on each); tty7 = graphics (nwm via `KDSETMODE` +
`VT_SETMODE(VT_PROCESS)` + release/acquire signals + `VT_RELDISP`). The PTY path (`/dev/ptmx`/`nterm`)
is untouched — VT multiplexes only the physical console.

**Hard-won gotchas (don't relearn):**
- VtManager is built with `new` in Kernel.cpp, NOT a file-scope/function-local global — its
  `RecursiveSpinlock` needs its `ownerCpu=-1` from a real ctor run (NanOS runs no global ctors; a
  function-local static would also need `__cxa_guard_*` which doesn't exist). `[[no-shortcuts]]`
- `/dev/ttyN` is mode **0666**: a non-root login shell (bash as jan) reopens its tty (`ttyname`) for
  readline; with the Linux-default 0620 (chown-to-user-at-login, which NanOS /dev can't do) it got
  EACCES → EOF → bash exited. This cost a long debug — the symptom was "bash logs out right after the
  prompt with no read".
- The getty must reset SIGTTIN/SIGTTOU/SIGTSTP to SIG_DFL before exec (init ignores them for itself;
  inherited SIG_IGN breaks the shell's job control).
- `/dev/ttyN` device reads are NON-blocking (`-EAGAIN`); the syscall dispatch drives blocking via
  `waitQueue()` — same contract as pipe/pty.
- All fb output funnels through VtManager (serial mirror injected = `arch::consoleSerialOut`, so the
  active VT + kernel console reach the headless serial log; per-VT fbcons don't touch serial).
- **F7 black-desktop was NOT a VT bug** (commit e639ce0). Two bugs in nwm's self-pipe wakeup (the fd a
  VT acquire/release signal writes to wake the poll loop): (1) `pipe(g_wakefd)` was never set
  O_NONBLOCK, so the per-iteration drain `while(read()>0)` blocked on the empty pipe and stalled the
  loop; (2) that drain lacked the `(int)` cast every other read-drain in nwm has — libc read() returns
  32-bit int but `<unistd.h>` prototypes it ssize_t, so an uncast -1/EAGAIN widens to 0xFFFFFFFF (>0)
  and spins forever. Fix = set both self-pipe ends nonblocking + `(int) read(...) > 0`. The KERNEL VT
  handoff (KDSETMODE→KD_GRAPHICS, acquire signal, sigreturn RESTART/KEEP) was correct all along. The
  general lesson: `(int)`-cast EVERY libc read()/write() return before a `>0`/`<0` test in userland
  (ssize_t-vs-int ABI mismatch). Note: nwm's SIGUSR1/2 come from picolibc `<signal.h>` (=30/31, NOT
  the kernel's 10/12) — self-consistent since nwm both registers and tells the kernel acqsig=31.

**Verified (QEMU, screendumps):** Ctrl+Alt+F1↔F2 switch + exact restore on return; Ctrl+Alt+F7 hands
the fb to nwm and the **desktop renders reliably (3/3 boots, non-black)**; Ctrl+Alt+F1 repaints text.

**Follow-ups DONE (the two that remained):**
- **isConsole per-fd → VT binding** (commit 97b8d50, Task 3.3): the inherited fast-path fds 0/1/2
  gained an `int vt` (default VT1, propagated by fork/dup); `Syscalls::read/write` route through
  `g_vtmgr->vt(fd.vt)` (the manager's locked write funnel) instead of reading the *active* VT +
  writing the global console. Falls back to `arch::inputRead`/console sink pre-VT + in host tests
  (g_vtmgr is null there → existing tests unaffected). Accessor `consoleVt(fd)` + binding test.
- **Unified `/dev/tty` (VT + pty)** (commit c87ed94): replaced the VT-only `Process::cttyVt` index
  with a stable `CharDevice* cttyDev` set by `TIOCSCTTY` on whichever tty the session adopts (a
  `VtTty` OR the `PtySlave`). New `ControllingTty` device (drivers/VtTty.*) = `/dev/tty`, forwards
  every op to `cttyDev` (-ENXIO when none = correct for a daemon). `PtySlave::ioctl` now handles
  TIOCSCTTY (login_tty already issued it); nwterm adopts pts0 (its "no TIOCSCTTY" comment was
  stale). Tests in tests/test_vttty.cpp cover both VT-ctty and pty-ctty delegation. Lesson: a
  controlling terminal is a *device pointer*, not a VT index — that's what unifies VT and pty.

**Graphical-VT login greeter `nwlogin` (commit 2190004)** — Linux display-manager model for tty7:
init runs `user/nwlogin.c` AS ROOT on the graphics console; it authenticates against /etc/shadow
(getpwnam/getspnam + crypt — same account DB as the text logins; jan/jan works), drops privilege
(initgroups+setgid+setuid) and execs nwm AS THE USER, so **the desktop no longer runs as root**.
Built via DYN_DEPS, in `X64_GUI_PROGS`, installed to /nanos/bin; init's `spawn_nwm` execs the greeter
(falls back to nwm directly only if the binary is absent). Two hard-won kernel fixes this needed:
- **LineDiscipline ECHO gate**: the cooked line discipline echoed UNCONDITIONALLY (passwords leaked
  on the text logins too). Added `LineDiscipline::setEcho` + `VtConsole::setEcho`, wired from VtTty
  TCSETS (apply AFTER setRaw — setRaw rebuilds the line discipline, resetting echo to on). Greeter
  reads the password with ECHO off.
- **`VtManager::acquireIfActive`**: nwm DEFERS drawing until the acquire signal (SIGUSR2), which only
  fired on a VT *switch-in*. Via the greeter, nwm is exec'd onto the ALREADY-ACTIVE tty7 (you switched
  to F7 to log in first) → no switch → no acquire → nwm hung black forever. Now VT_SETMODE(VT_PROCESS)
  on the active graphics VT delivers the acquire signal immediately (greeter→compositor handoff). General
  lesson: a compositor that waits for an acquire signal must also be handed the display when it claims
  an already-active VT. Verified in QEMU: F7 greeter prompt → wrong pw rejected → jan/jan → full nwm
  desktop renders (856k non-bg px). Moves toward [[nanos-user-permissions]] "init→login" goal.

Relates to [[nanos-terminal-and-deferred-scheduler]],
[[nanos-graphics-framebuffer]], [[nanos-smp-multicore]], [[no-shortcuts-on-foundations]].
