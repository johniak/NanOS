---
name: nanos-multiprocessing-roadmap
description: NanOS multiprocessing roadmap — stages 1-5 DONE (fork/exec/wait/exit + signals & job control); full doc in repo
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NanOS is building Linux-style multiprocessing with Unix `fork()`. The full, living roadmap
(stages, key design decisions, file locations, loose ends) lives in the repo at
**`docs/superpowers/ROADMAP.md`** — read it first when resuming.

Status:
- ✅ Stage 1: paging foundation.
- ✅ Arch refactor: Linux-style MI/MD split (`arch/x86/` behind `<arch/...>` contracts, `make check-arch`).
- ✅ Stage 2: init runs in **ring 3** with its own per-process address space, syscalls via `int 0x80`, isolated.
- ✅ Shell + coreutils (side-project): **`nsh`** shell boots; runs **verbatim sbase `cat`/`ls`** on a
  ported **picolibc**. Added `SYS_spawn` (synchronous nested ring-3 exec, 2nd kernel stack), argv on the
  user stack, cooked blocking stdin (keyboard line discipline), `SYS_stat` + ext metadata (`ls -l` real).
  Userland libc = picolibc (built in Docker) + glue under `user/libc-glue/` (syscalls/sbrk/dirent/cwd/pwd-grp);
  sbase vendored verbatim under `user/third_party/sbase/`. Spec/plan: `docs/superpowers/{specs,plans}/2026-06-07-*`.
- ✅ Synthetic root FS: `/` is an in-memory `SynthFs` (NOT a physical volume; deliberately
  non-Unix). Holds `/disks` (volumes), `/dev` (null/zero/random), `/proc` (uptime). The disk
  mounts at `/disks/main`; programs live at `/disks/main/bin`. `fs/SynthFs.*` + `Vfs::mount(mp,
  FileSystem*)` overload. Spec/plan: `docs/superpowers/{specs,plans}/2026-06-07-synthetic-root-fs*`.
- ✅ Stage 3: preemptive round-robin scheduler + `task` abstraction. MI `Scheduler`
  (kernel/Scheduler.*) + arch `switch.S`/`sched_x86.cpp` (context switch = callee-saved
  + esp + live CR3; `archTaskBootstrap`). 1000 Hz PIT (IRQ0) preempts; idle (PID 0) +
  init/nsh (only ring-3 task) + kernel clock thread; blocking read does block/wake
  (no hlt-spin); real `/proc/uptime`. Removed the broken `MultiTasking`. Spec/plan:
  `docs/superpowers/{specs,plans}/2026-06-07-stage3-scheduler*`.
- ✅ Stage 4: `fork`/`exec`/`wait`/`exit` — Unix process model on the **trap-frame +
  scheduler** model (a process is a scheduler task running until `exit()`; syscalls
  `iret`-return; blocking via `Scheduler::block/wake`). `archEnterUser` (CR3 + iret to
  ring 3, no return), `execve` rewrites the live trap frame (`archFrameToUser`), `fork`
  = eager copy (`mmuCopyAddressSpace`) + `archForkChild` → `ret_from_fork` (ISR tail in
  isr.S) irets the child with eax=0, `waitpid` blocks + reaps zombie, `exit`→`procExit`
  (selective `mmuFreeAddressSpace`, zombify, wake parent). Per-process `esp0` (scheduler
  sets TSS.esp0 per switch), task slots recycled (`TASK_FREE`). `kernel/Process.*` =
  process table + reap bookkeeping (host-tested). PID 1 execs nsh; **nsh runs commands
  via fork/exec/wait**; `SYS_spawn` removed. The old `g_userCtx`/`nx_longjmp`/`spawnUserImage`
  are gone. Spec/plan: `docs/superpowers/{specs,plans}/2026-06-07-stage4-fork-exec-wait*`.
- ✅ Stage 5: **signals + job control** (Linux-style, real subsystem — not a force-kill).
  MI core `kernel/Signal.*` (pending/blocked masks + disposition table; pure, host-tested).
  `kill(37)`/`signal(48)`/`sigreturn(119)`/`sigprocmask(126)`. Delivery at return-to-ring-3
  (syscall + IRQ return, `(cs&3)==3`): default → terminate (`WIFSIGNALED`) or stop;
  **catchable handlers** run in ring 3 via a user-stack sigframe (`archPushSignalFrame`,
  usermode_x86.cpp) + libc trampoline (`user/sigtramp.S`→sigreturn). Blocking `read`
  **restarts** (SA_RESTART, default-on like glibc `signal()`): returns internal `-ERESTARTSYS`,
  delivery rewinds eip to re-issue `int 0x80` or rewrites to `-EINTR` per a per-signal restart
  flag. Interruptibility gated on `sigHasInterrupt` (ignored SIGCHLD must NOT EINTR a wait —
  that was the job-control bug). Tty cooked-mode Ctrl+C→SIGINT, Ctrl+\→SIGQUIT, Ctrl+Z→SIGTSTP
  via `consoleSignal` (KeyDecoder tracks Ctrl). Job control: `TASK_STOPPED`+`procStop`,
  SIGCONT resume, `waitpid` WNOHANG/WUNTRACED + `ProcTable::reapStopped`, nsh `jobs`/`fg`/`bg`.
  `user/sigtest.c` is the QEMU test program. Spec: `docs/superpowers/specs/2026-06-08-signals-job-control-design.md`.
- **The multiprocessing roadmap (stages 1–5) is COMPLETE.**
- ⬜ later: `.ndl` dynamic-library loader (libc as a loadable lib), `.nkext`, ext write, getdents64 cursor.

Work is on branch `dockerized-build` (not pushed). Verify with `make build` / `make test`
(≥90% lcov) / `make check-arch` / QEMU headless screendump (ring-3 proof = `int 0x80` at
`cpl=3`). See also [[no-claude-attribution-in-commits]].
