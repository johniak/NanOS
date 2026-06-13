# NanOS Pipes & Time

Two small but load-bearing kernel pieces: the **pipe** (the byte FIFO behind `pipe(2)`, and the
ring model the PTY reuses) and the **wall clock** (boot epoch from the RTC + the monotonic scheduler
tick). Both are machine-independent and host-tested; blocking and the RTC read live outside them.

---

## 1. Pipes (`kernel/Pipe.h`)

A `Pipe` is a fixed-size ring-buffer **byte FIFO** with separate open-end refcounts — the object
behind a `pipe(2)` pair, and the model the PTY (@docs/terminal.md §5) reuses. It is a **pure data
structure**: no scheduler/arch dependency, so it is host-tested (`tests/test_pipe.cpp`); blocking is
the dispatch's job, not the pipe's.

- **Ring** — `write(src, n)` / `read(dst, n)` move up to `n` bytes and return the count moved
  (`0` = full / empty). `CAP = 64 KiB` — deliberately *not* 4 KiB: a windowed app's framebuffer
  COMMIT (a full redraw is ~770 KiB of pixels through the request pipe, @docs/windowing.md §2) would
  ping-pong ~190 times against a 4 KiB ring, each block costing a scheduler round-trip (brutal under
  QEMU TCG); a 64 KiB ring cuts the round-trips ~16×. Pipes are heap-allocated and few, so the bytes
  are cheap.
- **EOF via refcounts** — `addReader/addWriter/dropReader/dropWriter` track the open ends (`pipe()`
  opens one of each, `dup()` bumps, `close()` drops). `atEof()` = drained **and** no writer left, so
  a read on an empty pipe returns `0` (EOF) only once all write ends are closed — otherwise the
  caller blocks.
- **Blocking** — `waitQueue()` exposes a `WaitQueue` where readers (empty) / writers (full) park;
  the dispatch wakes them after any read/write/close changes readiness, instead of re-polling each
  tick (@docs/scheduler.md §4).

A pipe fd is one of the uniform fd backings in the `Syscalls` core (file / pipe / socket / pty), so
`read`/`write`/`close`/`dup`/`poll` work on it like any other fd (@docs/syscalls.md §2).

---

## 2. Time & the wall clock (`kernel/Clock.h`)

NanOS has two notions of time:

- **Monotonic tick** — `Scheduler::ticks()`, the 1000 Hz timer count since boot (@docs/scheduler.md
  §3). It drives `nanosleep`, poll/select timeouts, the TCP timers, and `/proc/uptime`.
- **Wall clock** — seeded once at boot: `setBootEpoch(arch::rtcEpoch())` samples the platform RTC
  (x86: the CMOS RTC, `arch/x86/cpu/cpu_x86.cpp`, behind `<arch/cpu.h>`), and `wallClockSeconds()`
  returns `bootEpoch + ticks/1000`. It lives in the MI layer so filesystem code can timestamp inodes
  (`mtime`/`ctime` on write, `utimes` "now") without reaching the arch directly (@docs/filesystem.md
  §5). It returns 0 until the epoch is set (e.g. host tests with no timer) — harmless for on-disk
  timestamps.

Surfaces:

- **`clock_gettime`(265)** returns the time to userland (@docs/syscalls.md); the userland clock is
  monotonic and feeds picolibc / ported apps (a `clock_gettime` monotonic fix was needed for the
  network ports).
- **`/proc/uptime`** = `ticks/1000`; **`/proc/stat` `btime`** = now (RTC) − uptime
  (`fs/SynthFs.cpp`).
- **`getrandom`(355)** and the CSPRNG also mix the RTC/tick at seed time (`kernel/Csprng.cpp`).

**Key files:** `kernel/Pipe.h`, `kernel/Clock.h`, `arch/x86/cpu/cpu_x86.cpp` (`rtcEpoch`),
`fs/SynthFs.cpp` (uptime/stat), `tests/test_pipe.cpp`.
