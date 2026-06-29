---
name: nanos-pthread-port
description: "Full POSIX pthread support on NanOS — musl pthread on picolibc, all 23 plan tasks done + QEMU-verified"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

Full POSIX threads run on NanOS: vendored musl pthread (`user/libc-glue/pthread/`) on picolibc inside `libc.ndl`, 1:1 kernel-thread model (each pthread = a kernel Task+Thread sharing the process AddressSpace + fd table). Branch `feat/pthread`; plan at `docs/superpowers/plans/2026-06-15-pthread.md` (all 6 phases / 23 tasks done, each through spec+code-quality review gates + QEMU). Built via the subagent-driven-development workflow.

What landed: futex(2); clone/set_thread_area/gettid/set_tid_address/exit_group; one reloaded GDT descriptor for GS TLS; per-thread errno via `__errno_location`; 64-bit signal masks + `rt_sig*` ABI + tgkill + per-thread delivery; create/join/detach + mutex/cond/rwlock/barrier/spin/once/keys/sem + cancellation; fork/exec collapse-to-one-thread. Gate programs: `user/pthrstress.c` (3200-thread churn) + `user/pfract.c` (parallel Mandelbrot). Docs: `docs/en/threads.md`.

Non-obvious facts worth recalling:
- A **real `munmap` (SYS_munmap) was added** to the kernel during Task 6.2 — unmaps PTEs, frees frames, and recycles VA via a per-process mmap free-list (`Process::mmapFree`, idempotent union-merge). Before this, munmap was a no-op and the 64 MiB mmap window exhausted after ~457 leaked thread stacks (the stress test caught it).
- **Deferred cancellation only** (no SA_SIGINFO/ucontext); async is best-effort at cancellation-point granularity. Cancellation points are futex-based waits only (cond/sem) — picolibc read/sleep are NOT cancellation points.
- **Detached-thread stacks leak until process exit** (a detached thread can't unmap its own running stack; musl `__unmapself` not vendored). Joined threads reclaim fully.
- **No `pthread_atfork`** — fork in a multithreaded process is only safe if the child execs immediately (bash pattern).
- Uniprocessor: concurrency, not parallelism. See [[nanos-terminal-and-deferred-scheduler]] (don't reintroduce IRQ-handler switching) and [[nanos-multiprocessing-roadmap]].
