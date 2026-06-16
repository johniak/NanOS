# NanOS Threads (pthreads)

NanOS runs real POSIX threads: the vendored musl pthread implementation on top of picolibc,
shipped inside `libc.ndl`, over a 1:1 kernel-thread model. A program that links `-lpthread` on
Linux runs unchanged here. The gate programs are `user/pthrstress.c` (a deterministic heavy
stress test) and `user/pfract.c` (a parallel Mandelbrot renderer driven by a real
mutex+condvar thread-pool work queue).

## Model: 1:1 kernel threads

Each `pthread` is a kernel **Task + Thread** pair. All threads of a process **share**:

- the process **address space** (`AddressSpace` / page tables) — one heap, one set of mappings,
- the **file-descriptor table**,
- the process-wide **signal dispositions** (the `sigaction` handlers).

Each thread has its **own**:

- kernel stack and register/`TrapFrame` state,
- user stack (allocated by pthread_create from the shared heap),
- TID (`gettid`), TCB / TLS block, and `errno` (`__errno_location` returns a per-thread slot),
- signal **mask** (the blocked set is per-thread; dispositions are per-process).

Threads are created with `clone(2)` (CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_THREAD|CLONE_SETTLS|…),
exit with `exit(2)`, and the whole process exits with `exit_group(2)`. `pthread_join` blocks on
a futex on the child's TID (the kernel writes the clear_child_tid word and wakes joiners on exit).

## Uniprocessor: concurrency, not parallelism

NanOS is single-core. Threads **interleave** on the one CPU under the scheduler; they do not run
simultaneously on multiple cores. The parallel Mandelbrot in `pfract.c` therefore divides the
*work* across worker threads but computes it on one core — the wall-clock result is the same as a
serial render. This is why `pfract`'s checksum (a fold of every cell's escape-iteration count) is
a deterministic correctness proof: the work queue only decides *who* computes each row, never
*what* the answer is.

## TLS: one reloaded GDT descriptor

Thread-local storage uses the i386 GS-segment convention. `set_thread_area`/`CLONE_SETTLS`
install the thread's TLS base into a **single GDT descriptor**; on each context switch the
scheduler reloads that descriptor (and `%gs`) for the incoming thread, so `%gs:0` always points
at the current thread's TCB. musl's `__pthread_self()` and the per-thread `errno` ride on this.

## Synchronization: futex-backed

All blocking primitives are built on the `futex(2)` syscall (a host-tested `FutexTable` over the
scheduler's wait/wake): `pthread_mutex`, `pthread_cond`, `pthread_rwlock`, `pthread_barrier`,
`sem_t`, and `pthread_once`. Uncontended fast paths stay in userspace (atomic CAS); only
contention enters the kernel. `pthread_spin_*` is a pure userspace spin (no futex).

## Supported API surface

- **Lifecycle:** `pthread_create`, `pthread_join`, `pthread_detach`, `pthread_exit`,
  `pthread_self`, `pthread_equal`, attributes (`pthread_attr_*`, including stack size).
- **Mutexes:** `pthread_mutex_*` (normal / recursive / errorcheck), `PTHREAD_MUTEX_INITIALIZER`.
- **Condition variables:** `pthread_cond_*` (signal/broadcast/wait/timedwait).
- **Read-write locks:** `pthread_rwlock_*`.
- **Barriers:** `pthread_barrier_*`. **Spinlocks:** `pthread_spin_*`.
- **One-time init:** `pthread_once`. **TLS keys:** `pthread_key_create/delete`,
  `pthread_setspecific/getspecific` (with destructors run at thread exit).
- **Semaphores:** `sem_init/destroy/post/wait/trywait/timedwait/getvalue` (`<semaphore.h>`).
- **Cancellation:** `pthread_cancel`, `pthread_setcancelstate/type`, `pthread_testcancel`,
  `pthread_cleanup_push/pop`.

## Signals

Signals are 64-bit (`rt_sigaction`/`rt_sigprocmask`/`rt_sigpending` ABI). Delivery is
per-thread: `tgkill(2)` targets a specific TID, and a signal sent to the process is delivered to
a thread that has it unblocked. Cancellation is implemented on top of this via an internal
`SIGCANCEL`.

## Limitations (accurate to what is built)

- **Deferred cancellation only.** `PTHREAD_CANCEL_DEFERRED` works at cancellation-point
  granularity. `PTHREAD_CANCEL_ASYNCHRONOUS` is best-effort — it is honoured at the next
  cancellation point, not truly asynchronously, because there is no `SA_SIGINFO`/`ucontext`
  machinery to unwind from an arbitrary instruction.
- **Cancellation points are futex-based waits only.** `pthread_cond_wait`/`timedwait` and the
  semaphore waits route through musl's `__syscall_cp` and *are* cancellation points. picolibc's
  blocking calls (`read`, `write`, `sleep`, …) are **not** routed through `__syscall_cp` in this
  hybrid libc, so they are **not** cancellation points — a thread blocked in `read()` will not
  cancel until it returns and hits an explicit `pthread_testcancel()`.
- **No `pthread_atfork`.** `fork()` in a multithreaded process gives the child only the calling
  thread (POSIX), but locks held by other threads at fork time stay locked in the child. Holding
  a libc lock across `fork()` is therefore unsafe unless the child **execs immediately** (the
  common bash-style pattern, which is exercised by `pthrtest`'s fork/exec sub-test).
- **No `SA_SIGINFO`/`ucontext`** for signal handlers (no siginfo payload, no machine context).
- **Uniprocessor** — no real parallel speedup (see above).
- **Detached-thread stacks leak until process exit.** `munmap` is real now (`SYS_munmap`
  frees the frames *and* recycles the VA via a per-process mmap free-list), so `pthread_join`
  reclaims a joined thread's stack+TCB and a long create/join churn recycles the finite 64 MiB
  mmap window. A **detached** thread, however, cannot unmap its own running stack and nobody
  joins it, so its stack+TCB are reclaimed only at process exit. The portable fix is musl's
  `__unmapself` (an i386 trampoline that switches to a tiny shared stack, munmaps the dead
  thread's mapping, then `SYS_exit`s); it is not vendored here. Programs that JOIN their
  threads (the common case, and what `pthrstress` exercises) reuse VA without bound.
- **Per-process mmap free-list is fixed-size** (`Process::NMMAPFREE`, 64 entries). If a process
  munmaps more distinct, non-coalescing ranges than that without re-mmapping, the excess frees
  still reclaim physical RAM but leak the VA (logged once). Ample for thread churn, where each
  join frees a region the next create immediately reuses.

## Verification (QEMU)

- **Stress (`pthrstress`):** 32 worker threads hammering a mutex-protected counter to `640000`
  (exact — no lost updates), a condvar producer/consumer, and **100 rounds of 32-thread
  create/join churn** (3200 thread lifecycles). Result: `pthrstress: 32 threads,
  counter=640000, churn 100 rounds ok`, 0 CPU faults. This churn is what surfaced the original
  mmap-stack leak (fixed by real `munmap` + VA reuse, above).
- **Parallel render (`pfract`):** `pfract: 8 workers, 1540 cells, checksum=0x… ok` with the
  Mandelbrot set drawn in ASCII — a real mutex+condvar thread-pool work queue.
- **Kernel-stack RAM headroom:** `/proc/meminfo` `KHeapFree` is **identical before and after**
  the 3200-thread churn (e.g. `32350 kB` → `32350 kB`). Each thread's 32 KiB kernel stack is
  heap-allocated on create and fully reclaimed when the scheduler reaps the DONE task, so the
  kernel heap returns to the byte with no leak; peak concurrent kernel-stack use (~32–64 live
  threads ≈ 1–2 MiB) stays well within the heap. The task-slot ceiling
  (`MAXTASKS = ProcTable::MAX + 8`) is ample and was deliberately **not** lowered.

## Follow-up

A Polish translation `docs/pl/threads.md` is a pending follow-up (docs are bilingual; see
`docs/en/README.md`); it is not required to land this change.
