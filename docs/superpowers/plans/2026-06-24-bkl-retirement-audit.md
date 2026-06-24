# BKL Retirement — Audit & Rework Plan (Phase 4, Task 15)

**Status:** investigation only — no code changed. For review before execution.
**Branch:** `feat/smp` (Tasks 11–14 already committed: frame alloc, heap, proc table, device
manager, VFS, block cache, journal all independently locked; deadlock-safe IPI TLB shootdown).

---

## TL;DR

The plan's Task 15 (“remove the BKL; add a runqueue lock + console lock + net lock”) **understates
the work by an order of magnitude.** A full audit finds **~15 distinct shared structures that the
BKL is the *only* thing protecting today.** Removing the BKL means giving each its own lock, getting
the scheduler context-switch handoff right *without* the BKL, and establishing a global lock-ordering
discipline so the new locks don't deadlock against each other or against the TLB-shootdown poll.

**Recommendation: split Task 15 into 5 reviewable sub-tasks (15a–15e), land and verify each under the
still-present BKL, and flip the BKL off only in the final step.** Each lock can be added while the BKL
still serializes everything (so it's a no-op safety-wise until the end), exactly like Tasks 11–14.
The BKL stays as the backstop until the last structure is covered; the final commit removes it.

This is the riskiest work in the project (the scheduler handoff is the code your notes flag as the
source of the triple-fault / wake-during-switch-out bugs). The phased approach keeps every
intermediate state shippable and verifiable.

---

## 1. What is already covered (Tasks 11–14, committed)

| Structure | Lock | Notes |
|---|---|---|
| Frame allocator | `FrameAllocator::m_lock` (Spinlock) | Task 11 |
| Kernel heap | `Heap::m_lock` (Spinlock) | Task 11 |
| Process table `g_procs[]/g_threads[]` | `g_procLock` (RecursiveSpinlock, IRQ-saving) | Task 12 |
| Device manager | `devLock` (Spinlock) | Task 12 |
| VFS (mount table + path routing + all FS ops) | `g_vfsLock` (RecursiveSpinlock, non-IRQ) | Task 13 |
| Block cache slots | `BlockCache::m_lock` (Spinlock, non-IRQ) | Task 13 |
| JBD2 transaction | `ExtFilesystem::txLock` (Spinlock, non-IRQ) | Task 13 |
| Net RX backlog | IRQ-save guard (pre-existing) | not BKL-only |
| Cross-CPU TLB flush | `smpTlbShootdown` + `smpPollShootdown` in every spinlock spin | Task 14 |

The **per-CPU** scheduler/process state is correct by construction (each CPU owns its slot, indexed by
`smpThisCpu()`): `g_curTask[]`, `g_cpuIdle[]`, `g_switchFrom[]`, `g_needResched[]`, `g_slice[]`,
`g_curProc[]`, `g_curThr[]`, the per-CPU tick accounting arrays. These need **no** lock.

---

## 2. What is still BKL-only (the Task-15 work list)

Grouped by proposed sub-task. “Context” = which execution contexts mutate it (T=thread/syscall,
I=hardware IRQ, S=net softirq/timer-thread). All file:line refs from the audit.

### 15a — Scheduler runqueue (`g_rqLock`)  ⚠️ the hard one
- `g_tasks[]`, `g_ntasks`, task `state`/`runningCpu`/`wakeAt`/`waitNext` — `kernel/Scheduler.cpp:26-27`.
- Global counters touched by `onTick`: `g_ticks` (41), `g_ctxt` (42), `g_load[]` (43).
- Wait queues (`WaitQueue` intrusive lists) — added/removed by `sleepOn`/`wakeAll`.
- Mutators: `schedule()` (claim READY→RUNNING, 260-275), `wake()` (395, **I/S**), `block()`/`sleepOn()`
  (384/420, **T**), `onTick()` (timed wakeups + reap, 312-321, **I**), `wakeAll()` (469, **I/S**),
  `finishSwitch()` (232–233), `reap()`/`create()`/`allocSlot()`.
- **This is the structure whose lock must be released across `archContextSwitch` (the handoff).**
  See §3 for the detailed rework.

### 15b — Per-process / per-thread syscall + signal state
- **FD table** `Syscalls::fds[MAXFD]` — `kernel/Syscall.h:121`. Per-process, **shared across threads
  (CLONE_FILES)** → two threads of one process on two CPUs race on `open/close/read/dup` (**T**).
- `m_cwd`, `m_umask`, `consoleTermios` — `Syscall.h:124-126` (**T**).
- **Pipes** `Pipe` ring buffer + counts — `kernel/Pipe.h:66-69`. Shared via fds; reader on one CPU vs
  writer on another corrupt `m_head/m_tail/m_count` (**T**).
- **Futex table** `g_futex` buckets — `kernel/SyscallDispatch.cpp:270` / `kernel/Futex.h:46`. WAIT on
  one CPU vs WAKE on another corrupt the bucket FIFO (**T**, WAKE can come from any process).
- **ProcSignals** (pending/handlers/restart) — `kernel/Signal.h:120`. `signal()`/`rt_sigaction` (**T**)
  races `sigPost` from `kill()` on another CPU and `tickRealTimers` SIGALRM (**I**).
- **ThreadSignals** (pending/blocked) — `kernel/Signal.h:106`. `sigprocmask` (**T**) races `tgkill`
  posting + the thread's own `signalDeliver`.
- Job control globals `g_foregroundPid`, `g_consolePgrp` — `kernel/Exec.cpp:24,31`. Written by
  `waitpid`/`tcsetpgrp` (**T**), **read by `consoleSignal` from the keyboard IRQ** (**I**) → IRQ reads
  a half-updated value.

### 15c — Exec / dynamic loader (serialize)
- **Exec staging window** `STAGE_BASE = 0x800000` (32 MiB) — `kernel/Exec.cpp:49-56`. A **single global
  buffer**; two concurrent `execve`s on two CPUs clobber each other's image (**T**). Highest-impact.
- Dynamic loader globals `g_libs/g_vfs/g_space/g_nextBase` — `kernel/DynLoader.cpp:61-96` (**T**).
- (Boot-only, no lock needed: `KextLoader g_mods[]`, `KernelExports g_nextInput/g_msi*` — single-threaded
  init. Document as boot-only.)

### 15d — Console (`g_consoleLock`)
- VGA buffer + `cursorX/cursorY` — `arch/x86_64/drivers/console_x86_64.cpp:25-27`. 4 CPUs printing
  concurrently interleave cells + desync the cursor (**T**, also kprintf from **I**).
- FbConsole `g_fb` grid/parser — same file :34-35.
- Serial `g_serialReady`/UART — :50.
- `itoa` static buffers in `drivers/Console.cpp:63,92` — shared return buffers (**T**).

### 15e — Network stack (`g_netLock`, coarse)
- Socket table `g_socks[128]` + rx rings — `net/Socket.cpp:20`. `socketDeliver` (**S**, from `tcpRx`)
  vs `socketRecvFrom` (**T**).
- `g_nextEphemeral` (:22), `g_isnCounter` (`net/Tcp.cpp:89`) — non-atomic counters.
- **TCP control blocks** `g_tcbs[64]` — `net/Tcp.cpp:86`. The classic three-way race: `tcpRx` (**S**) vs
  `tcpSend/tcpRecv` (**T**) vs `tcpTick` (timer). Sequence numbers + 8 KiB send/recv buffers.
- Route table `g_routes[]` — `net/Route.cpp:8`. ARP cache + pending `g_cache/g_pend` — `net/Arp.cpp:12,16`
  (**S** + **T**). Stats `g_netStats` — `net/NetStats.cpp:9` (lost-update only; cosmetic).
- **Open question to confirm in 15e:** is NIC RX delivered on a hard IRQ or a softirq/kernel thread?
  The backlog handoff (`net/NetDevice.cpp:13`) is already IRQ-guarded, which suggests RX *processing*
  runs in thread/softirq context. That determines whether `g_netLock` may be a plain `SpinGuard`
  (thread-only) or must be `SpinIrqGuard` (taken from a hard IRQ). Must verify before locking.

### Also BKL-only, lower urgency
- `/proc` generator **static buffers** in `fs/SynthFs.cpp` (gen_stat/cpuinfo/net_* etc.). Two readers of
  the same node interleave the shared `static char s[]`. Fix: these are reached **under `g_vfsLock`
  already** (they're VFS reads) → *already serialized*. ✅ Confirm and document; likely no new lock.
- `g_ticks` increment: only the BSP PIT increments it (`onTick`); APs use `onTickLocal` which does **not**
  touch `g_ticks`. So it has a single writer already — covered by `g_rqLock` for the read-side consistency.

---

## 3. The scheduler handoff rework (15a, the crux)

Today `schedule()` runs at **BKL depth 1** and does (Scheduler.cpp:280-283):

```
g_bkl.exit();                                  // release so another CPU can enter the kernel
arch::archContextSwitch(&prev->kesp, next->kesp);   // saves prev->kesp, loads next
g_bkl.enter();                                 // far side: resumed task re-acquires
finishSwitch();                                // clear prev->runningCpu = -1 (now claimable)
```

The **`runningCpu` gate** (Scheduler.cpp:101-117) is what makes this safe: between the release and the
kesp-save, `prev` is left `TASK_RUNNING`/`runningCpu=oldcpu` so no other CPU claims it before its stack
pointer is saved. `pickReady` requires `runningCpu == -1`.

### Replacement: a dedicated `g_rqLock` (Spinlock, IRQ-saving)
`g_rqLock` is taken around **every** runqueue read/mutation and released across the switch exactly where
the BKL is today. It is a **leaf lock**: while holding `g_rqLock` we take **no other lock**, and we never
wait for `g_rqLock` while holding another lock (see ordering, §4). Concretely:

- `schedule()`: take `g_rqLock` (IRQ-save) at entry; pick+claim `next`; set per-CPU current; set
  `g_switchFrom[cpu]=prev`; **`g_rqLock.unlock()` + restore IRQs** right before `archContextSwitch`
  (mirror of today's `g_bkl.exit()`); on the far side **re-acquire** and `finishSwitch()`; release.
- `wake()`/`wakeAll()`/`resume()` (called from **IRQ**): wrap the state flip in `SpinIrqGuard(g_rqLock)`.
- `block()`/`sleepOn()`/`sleepUntil()`: the “save flags / set BLOCKED / schedule()” sequence takes
  `g_rqLock` for the flip; `schedule()` itself re-takes it (so make the flip + schedule path take it
  once, or use a non-recursive design where the caller flips state under `g_rqLock`, drops it, then
  calls a `schedule()` that re-takes — must avoid double-lock; see risk R1).
- `onTick()`: already runs in IRQ context; take `SpinIrqGuard(g_rqLock)` around the timed-wakeup scan +
  reap. **But `onTick` also calls `ProcTable::tickRealTimers` (takes `g_procLock`) and `Scheduler::wake`.**
  → ordering hazard, see §4 R2.
- The asm entry stubs (`syscall_entry64.S:46/50`, `irq64.S:82/99`, `isr64.S:87/93`) and
  `usermode_x86_64.cpp:112` lose their `bklEnter/bklExit` calls. `schedForkFinish` and `Scheduler::start`
  switch from `g_bkl` to `g_rqLock` for their pick/finish brackets.

### Why `g_rqLock` must be IRQ-saving and recursion-free
- IRQ-saving: `wake/wakeAll/onTick` fire from hardware IRQs; a same-CPU timer IRQ that grabbed `g_rqLock`
  while a syscall held it would self-deadlock (the classic). `SpinIrqGuard` prevents it.
- The TLB-shootdown poll (`smpPollShootdown` in `Spinlock::lock`'s spin) means a CPU spinning for
  `g_rqLock` still services shootdowns — good, no new deadlock there.
- Recursion: unlike `g_procLock`/`g_vfsLock`, the runqueue lock should **not** be recursive — the handoff
  relies on a single release fully dropping it across the switch (depth-1 invariant). Composing scheduler
  calls (e.g. `block()`→`schedule()`) must be restructured so the lock is taken exactly once, not nested.

---

## 4. Global lock-ordering discipline (deadlock avoidance)

With the BKL gone, multiple fine-grained locks can be held at once. Define a strict acquire order
(outer→inner); never acquire an outer lock while holding an inner one:

```
1. g_vfsLock            (recursive, non-IRQ)   — entered first by FS syscalls
2. txLock  /  BlockCache::m_lock                — under the VFS op (FS internals)
3. g_netLock            (coarse net)            — socket/TCP syscalls (NOT under VFS)
4. g_procLock           (recursive, IRQ-save)   — process table
5. subsystem leaf locks: fd-table, pipe, futex, signal, console, frame, heap
6. g_rqLock             (IRQ-save, LEAF)        — innermost; released across the switch
```

Key constraints surfaced by the audit:
- **R1 (block-under-lock):** `sleepOn` must **not** be called while holding `g_vfsLock` or `g_netLock`.
  The audit confirms blocking reads sleep at the **SyscallDispatch** layer *outside* the VFS op
  (`pollReady`/`waitQueueAt` each lock-and-release, then `Scheduler::sleepOn`). ✅ Holds today; must be
  preserved when adding `g_netLock` (socket recv must drop `g_netLock` before sleeping).
- **R2 (onTick fan-out):** the timer IRQ takes `g_rqLock` **and** calls `tickRealTimers` (`g_procLock`)
  **and** `wake`. Pick one order — `g_procLock` (4) before `g_rqLock` (6) — and make `tickRealTimers`
  *not* hold `g_procLock` across `Scheduler::wake` (it currently calls `wake` inside its proc scan).
  Either collect tasks-to-wake under `g_procLock`, release, then `wake` under `g_rqLock`; or make `wake`
  safe to call with `g_procLock` held (then `g_procLock`→`g_rqLock` order is fixed and must never invert).
- **R3 (signal post from IRQ):** `sigPost` from `kill`/IRQ touches `ProcSignals`/`ThreadSignals` while the
  target may be in `signalDeliver`. The signal lock must be IRQ-saving and ordered before `g_rqLock`
  (since delivery may `wake`).
- **R4 (console from IRQ):** kprintf can be called from an IRQ handler → `g_consoleLock` must be
  IRQ-saving (`SpinIrqGuard`).

---

## 5. Proposed phasing (each: add lock under the still-live BKL → host test + smoke-smp → commit)

Because every new lock nests *inside* the BKL until the final flip, each sub-task is individually safe
and verifiable (the BKL keeps serializing; the new lock is uncontended). Order chosen so the riskiest
(handoff) lands with the most surrounding scaffolding already in place.

- **15a** — Add `g_rqLock`; convert all scheduler mutators + the handoff. Keep BKL too (belt-and-braces).
  Verify the handoff invariants under `smoke-smp` + `smoke-smp-speedup`. *Highest risk.*
- **15b** — fd-table lock (per-`Syscalls`), pipe lock (per-`Pipe`), futex lock (per-bucket or global),
  signal locks (per-process + per-thread), job-control globals lock.
- **15c** — exec serialization (a global `g_execLock`, or per-process staging) + dynloader lock.
- **15d** — `g_consoleLock` (IRQ-saving) over VGA/fb/serial/itoa.
- **15e** — `g_netLock` (coarse) over sockets/TCP/ARP/routes/counters, after confirming RX context.
- **15f (the flip)** — remove `bklEnter/bklExit` from the three asm stubs + `usermode_x86_64.cpp`, drop
  `g_bkl` from the scheduler/`schedForkFinish`/`start`/`idleBody`. Run the **full** `verify64`
  (BIOS+UEFI+bigmem+e1000e+usb+smp+speedup) + `e2fsck`. Re-measure pfract speedup (should improve, since
  kernel work is no longer serialized). Confirm `/proc` static-buffer reads are covered by `g_vfsLock`.

Task 16 (docs) then records the BKL→fine-grained history.

---

## 6. Risks

- **Handoff regression (R-high):** getting `g_rqLock` release/re-acquire wrong around `archContextSwitch`
  reintroduces the wake-during-switch-out double-dispatch / triple-fault class. Mitigation: keep the
  `runningCpu` gate exactly as-is (it's lock-independent); change only *which* lock brackets the switch;
  add a host test for the `pickReady` claim invariant; keep the BKL in place through 15a–15e so any
  regression shows up while still serialized-enough to bisect.
- **Missed structure (R-high):** an unlocked shared structure → rare SMP corruption smokes miss.
  Mitigation: this audit is the checklist; add a `grep`-style review for `static`/global mutable state in
  `kernel/`, `net/`, `drivers/`, `arch/x86_64/` before 15f; run `smoke-smp-speedup` (real parallelism) +
  a new concurrent-fork/pipe/socket stress under MTTCG.
- **Lock-order inversion (R-med):** §4 ordering must be enforced; the `onTick`→`procLock`+`rqLock` and
  `signal`→`rqLock` paths are the live hazards.
- **Perf:** `g_netLock`/`g_consoleLock`/exec-lock are coarse; acceptable (correctness first; the plan
  itself calls per-inode/per-socket locking a “later optimization”).

---

## 7. Recommendation

Proceed **only** with the phased 15a–15f above, landing and verifying each under the BKL, flipping the
BKL off last. This is ~6 commits, not 1. Given the risk, I suggest reviewing/executing 15a (the handoff)
as its own reviewed step before committing to the rest. Alternatively, **bank Tasks 11–14** (already a
real, verified improvement) and treat full BKL retirement as a separate, scheduled effort.
