---
name: nanos-terminal-and-deferred-scheduler
description: NanOS terminal (PTY + nterm emulator) stages 1-6 done; scheduler uses deferred preemption
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NanOS gained a full userspace terminal (xterm model): kernel PTY (`drivers/Pty`) + line
discipline + termios, `pipe`/`dup2`/`poll`, a userspace VT emulator `user/term/nterm.c`
(mmap /dev/fb0, 8x16 font, CSI/SGR/256-colour/alt-screen parser) running `nsh` on the slave,
sessions + process groups + job control, and Linux-format `/proc` (stat/loadavg/cpuinfo/
version + pgid/sid in per-pid stat). All six planned stages are committed on `dockerized-build`.

**Most important non-obvious fact — the scheduler uses DEFERRED preemption** (commit c7f7249).
The original timer IRQ context-switched *inside* the handler and blocking syscalls busy-waited
on `arch::halt_or_hlt`; under heavy concurrent rendering this corrupted a CPU-bound task's
state. Fixed by the Linux `ret_from_intr` model: `Scheduler::onTick` only bumps the clock,
wakes I/O waiters, and sets a `need_resched` flag; the actual `schedule()` happens ONLY at safe
points — `irq.S` → `schedPreempt` on the return path to ring 3, or a voluntary
`Scheduler::ioWait()`/`block()` in the kernel. The kernel is never switched out at an arbitrary
ring-0 instruction. **Do not reintroduce timer-driven switching in the IRQ handler or
`halt_or_hlt` busy-wait loops in blocking syscalls** — kernel threads (idle/clock/init) must
yield via `ioWait()`. PTY writes are partial-write + dispatch-accumulated (lossless flow
control), never dropping bytes.

Verified via headless QEMU screendumps; host tests 233 passing, coverage gate ≥90% holds.
terminfo (xterm-256color) is deferred to a future ncurses port (no consumer yet; envp isn't
plumbed). Related: [[nanos-multiprocessing-roadmap]].
