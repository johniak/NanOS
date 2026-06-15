# NanOS full pthread (POSIX threads) — design

**Goal:** real multi-threaded software *just works* on NanOS. The priority is ABI fidelity with what
real apps link against — NPTL-style `clone`+`futex`+TLS — so a standard pthread library runs
unmodified. This fits NanOS's existing philosophy (Linux syscalls over `int 0x80`, real i386 numbers)
and its existing preemptive scheduler.

**Userland strategy:** port **musl's pthread (NPTL)** into the SDK libc (`user/libc-glue`), adapted to
picolibc — battle-tested correctness for the hard primitives (condvar sequencing, cancellation,
rwlock fairness) instead of reinventing them.

**Constraint:** NanOS is uniprocessor — this delivers **concurrency, not parallelism** (no speedup;
threads time-slice on one CPU). Correctness, not performance, is the win.

---

## Current model (what we build on)

- `Scheduler` is preemptive round-robin over `Task`s (kernel stack + saved `kesp` + state + back-pointer
  to its owning `Process`). "From the scheduler's view every task is a kernel thread." Context switch
  is arch (`<arch/sched.h>`); preemption is deferred (resched flagged on tick, applied on ret-to-ring3).
- A `Process` today = exactly one `Task` + one `AddressSpace` + one `Syscalls` (fd table) + signal
  state + brk/mmap + pgid/sid.
- No `clone`/`futex`/`set_thread_area`/TLS exists.

**Key insight:** the scheduler already does the hard part. A pthread is just **another `Task` that
shares its process's AddressSpace + fd table** — a 1:1 kernel-thread model.

---

## 1. Thread model (1:1)

A `Process` becomes a **thread group**. Each pthread is an additional `Task`.

- **Shared per thread-group (process):** `AddressSpace`, `Syscalls` (fd table), signal **dispositions**
  + process-pending set, `brk`/`mmap` windows, `pgid`/`sid`, cwd, `tgid` (= leader pid).
- **Per thread:** kernel stack (`Task` already), user stack, TLS block + GDT slot, `tid`, signal
  **mask** + per-thread pending, cancellation state (enabled/deferred/asynchronous + pending),
  CLEARTID address.
- `tid` is system-wide-unique (reuse the `Task` id space); the leader's `tid == pid == tgid`.
- Representation: a `Thread` record bound to a `Task` and a `Process`; `Process` keeps a thread list +
  live-thread count. `Process` fields that are inherently per-thread (signal mask, the active
  `Syscalls` "current" hooks) move to the `Thread`.

## 2. Kernel syscalls (Linux i386 semantics)

- **`clone(120)`** — the "share instead of copy" sibling of `fork`. For pthreads the flags are
  `CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|SETTLS|PARENT_SETTID|CHILD_CLEARTID|CHILD_SETTID`. Creates a
  new `Task` in the **same** `AddressSpace` + `Syscalls`, with `child_stack` as the user ESP, `tls` as
  the TLS descriptor, returns the new `tid` to the caller and `0` in the child. Refactor `fork` and
  `clone` onto one task-creation core (copy-address-space vs share-address-space is the only fork).
- **`futex(240)`** — `FUTEX_WAIT` (atomically: if `*uaddr == val` block, else `-EAGAIN`), `FUTEX_WAKE`
  (wake ≤`val` waiters), `FUTEX_REQUEUE`/`CMP_REQUEUE` (condvars), `FUTEX_WAIT_BITSET`/`WAKE_BITSET`,
  `FUTEX_PRIVATE` flag, optional `WAKE_OP`. Backed by a kernel hashtable keyed by the futex address →
  `WaitQueue`; reuses `Scheduler` block/wake + timed wakeup for timeouts. The atomic compare-and-block
  runs with interrupts/preemption disabled around the load+enqueue.
- **`set_thread_area(243)`** — allocate/update a per-thread GDT entry (base = TLS block); return its
  index; userland sets `GS = (index<<3)|3`. The running thread's TLS GDT entry is reloaded on context
  switch.
- **`set_tid_address(258)`** — record the CHILD_CLEARTID address; on thread exit the kernel writes `0`
  to `*ctid` and `FUTEX_WAKE`s it (how `pthread_join` observes exit).
- **`gettid(224)`, `tgkill(270)`** (per-thread signal), **`exit(1)`** = end *this thread*, **`exit_group(252)`**
  = end the whole group. `get_thread_area(244)` for completeness.

## 3. TLS (GS-based, i686 variant II)

The compiler emits `__thread` accesses GS-relative (variant II: TLS block below the thread pointer, TCB
at it, first word = self). `set_thread_area` backs GS with a per-thread GDT entry (small slot pool, or a
single slot reloaded per switch). **The TCB uses musl's `struct __pthread` layout** (holds `self`, `tid`,
errno, cancellation state, the join futex). **Integration point (designed explicitly): picolibc's
`errno` must resolve to the TCB's errno** so picolibc functions and app code share one per-thread errno —
either by routing picolibc's `__errno`/`errno` through the thread pointer, or by making the TCB the
single source and adapting picolibc's accessor.

## 4. Userland: musl pthread on picolibc (`user/libc-glue`)

Bring in musl's pthread sources, adapted to NanOS/picolibc:
- `pthread_create/join/detach`, `pthread_mutex_*` (normal/recursive/errorcheck), `pthread_cond_*`,
  `pthread_rwlock_*`, `pthread_barrier_*`, `pthread_spin_*`, `pthread_once`, `pthread_key_*` (TLS keys),
  `pthread_cancel`/`setcancelstate`/`testcancel`, `pthread_attr_*`, `sem_*`.
- Adapters: musl `__syscall` → NanOS `int 0x80` wrappers; `__clone` asm (sets up the child stack +
  entry); `__set_thread_area`; `struct __pthread`/thread-pointer setup; `__timedwait`/`__wait` on the new
  `futex`; `__lock`/`__unlock`; cancellation via `__syscall_cp` + a reserved `SIGCANCEL`.
- `pthread_create`: `mmap` the stack + TLS/TCB, fill the TCB, `clone(CLONE_*…, stack, &tcb->tid, &tcb, &tcb->tid)`.
- `pthread_exit`/return → `exit(1)`; kernel CLEARTID-futex-wakes joiners.

## 5. Signals + threads

Per-thread signal **mask** + pending; process-wide **dispositions** + process-pending. A process-directed
signal (`kill`) is delivered to any thread not blocking it; `tgkill` targets one thread. `pthread_cancel`
rides a reserved `SIGCANCEL`. A fatal signal / `exit_group` terminates every thread in the group.

## 6. Process lifecycle with threads

- `fork()` in a multithreaded process: POSIX — only the calling thread survives in the child (fresh
  single-thread process with a copied AddressSpace).
- `execve()` collapses the group to a single new thread.
- Thread exit frees its `Task` + user stack + TLS; the **last** thread frees the AddressSpace + Process.
  `exit_group`/fatal signal terminates all. The parent reaps the *group* (process); individual threads
  are joined via futex, never `wait()`.

## 7. Thread-safe libc (load-bearing, easy to under-scope)

Once threads exist, **picolibc malloc/stdio/errno + the libc-glue allocator must be thread-safe** or real
apps corrupt/deadlock. picolibc has retargetable locks (`__retarget_lock_*`) → wire them to the new
mutex/futex. `errno` becomes per-thread (TCB, §3). `FILE` locking (`flockfile`) becomes real. The
allocator (memory_manager / picolibc malloc) gets a lock. This is part of "full pthread," not optional.

## 8. Testing & no-shortcuts gate

- **Host:** the pure futex logic (hashtable/wait/wake matching, requeue), the thread-table/clone bookkeeping
  where pure.
- **QEMU:** a pthread test suite — create/join/detach, mutex contention, condvar producer/consumer,
  rwlock, barrier, `pthread_once`, TLS keys, cancellation — plus a **real `-lpthread` program** (e.g. a
  threaded tool, or curl rebuilt with the threaded resolver) as the "real apps work" gate, and a stress
  test (many threads + contention) with no deadlock/crash. `-d int` clean (no `v=08/0d/0e`).

## Risks

- musl↔picolibc coupling (TCB/errno) — the main integration risk; designed explicitly in §3/§7.
- futex atomicity under deferred preemption — bracket the compare-and-block with interrupts off.
- GDT TLS slots — finite; reload per switch.
- Uniprocessor — concurrency only (acceptable; correctness is the goal).
- Scope discipline — §7 (thread-safe libc) and cancellation are the parts most likely to be
  under-estimated; they are in-scope for "full."
