# NanOS Scheduler & Process Model

A preemptive, single-CPU, round-robin scheduler built on **deferred preemption**: the timer IRQ
never switches tasks itself — it only flags that a reschedule is due, and the actual context switch
happens at a well-defined safe point (the return to ring 3, or a voluntary yield in the kernel).
This is the Linux `ret_from_intr` model, and it is the heart of the design: the kernel is never
switched out at an arbitrary ring-0 instruction, so interrupt and syscall frames never interleave
on a task's kernel stack.

The code splits MI policy from MD mechanism: `kernel/Scheduler.{h,cpp}` owns the task table,
states, round-robin selection and the block/wake logic; the `<arch/sched.h>` contract supplies the
CPU context switch, the fabricated first-run stack, and the preemption timer (`arch/x86/cpu/
switch.S` + `sched_x86.cpp`). The process layer (`kernel/Process.*`, `kernel/Exec.cpp`) sits on
top. For how programs enter ring 3 and how signal frames are built see nxe-ndl.md §6; for the
network kernel threads see networking.md §3.

---

## 1. Task vs Process

Two distinct objects:

- **`Task`** (`kernel/Scheduler.h`) — what the scheduler runs: a kernel stack + a body function +
  saved context. From the scheduler's view *everything* is a kernel thread; one task (init) happens
  to enter ring 3, which is orthogonal to scheduling.
- **`Process`** (`kernel/Process.h`) — the Unix process: pid, address space, fd table, signal
  state, job-control group. A process owns one task.

They point at each other so each lookup is O(1): `Task::proc` (route a syscall to its process) and
`Process::task` (find the runnable task for a pid). `ProcTable::setCurrent(task->proc)` is called on
every switch so syscalls hit the right process without an O(n) scan.

```cpp
struct Task {
    unsigned kesp;        // saved kernel esp — the whole context lives on the stack
    unsigned esp0;        // top of this task's kernel stack (loaded into TSS.esp0 when it runs)
    TaskState state;
    void (*body)();
    int id;               // 0 = idle
    unsigned char* kstack;
    unsigned wakeAt;      // BLOCKED-with-deadline: tick at which onTick re-wakes it (0 = none)
    Process* proc;        // owning process back-pointer
    Task* waitNext;       // intrusive link while parked on a WaitQueue
};

enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_STOPPED, TASK_DONE, TASK_ZOMBIE, TASK_FREE };
```

| State | Meaning |
|---|---|
| `TASK_READY` / `TASK_RUNNING` | runnable / currently on the CPU |
| `TASK_BLOCKED` | waiting on a WaitQueue, a timed deadline, or an I/O retry |
| `TASK_STOPPED` | job-control suspended (SIGSTOP/SIGTSTP) — only `resume()` un-stops it |
| `TASK_DONE` | a kernel-thread body returned; lazily reaped by `onTick` |
| `TASK_ZOMBIE` | a process exited, awaiting the parent's `waitpid` |
| `TASK_FREE` | slot reusable by `findFreeSlot()` |

Task **slots** are a static array `g_tasks[MAXTASKS]` (`MAXTASKS = ProcTable::MAX + 8 = 1032`); the
expensive part — each task's **32 KiB** kernel stack (`KSTACK_SIZE`) — is heap-allocated on create
and freed on reap, so the live task count is bounded by RAM, not the array. (The stack is 32 KiB
because the ext-write + JBD2 journal path nests several 4 KiB block buffers and a keyboard IRQ can
land on top; a heap boundary canary turns an overflow into a clean panic.)

---

## 2. Deferred preemption — the core mechanism

### The timer tick does *not* switch

`Scheduler::onTick(fromUser)` (driven by the 1000 Hz PIT IRQ0) only:

1. `g_ticks++`;
2. accounts the tick to the running process (user vs system, by the ring it interrupted);
3. samples the load average every 5 s (Linux EWMA, `loadDecay`);
4. wakes any `TASK_BLOCKED` task whose `wakeAt` deadline has arrived, and lazily reaps `TASK_DONE`
   kernel threads;
5. sets **`g_needResched = true`** — but only on a quantum boundary or when a sleeper just woke
   (`shouldResched`), so two CPU-bound tasks don't trade the CPU (and flush the TLB) 1000×/s.

The quantum is `QUANTUM = 10` ticks (10 ms): a RUNNING task keeps the CPU until its slice expires,
unless an I/O completion / expired timer makes a higher-priority sleeper runnable, which preempts it
promptly.

### The switch happens at safe points only

```
timer IRQ0 ──► onTick(): g_ticks++, wake sleepers, set g_needResched   (NO switch)
                                │
        ┌───────────────────────┴───────────────────────┐
        ▼                                                 ▼
ret-to-ring3 from an IRQ                       voluntary yield/block in kernel
(irq.S tests saved CS & 3)                     (schedule / block / sleepOn / ioWait)
   call schedPreempt() ─► preempt():                     │
     if g_needResched: schedule()                        ▼
        └──────────────────────────────► schedule(): pick next + archContextSwitch
```

- **Involuntary preemption** happens *only* on the return path from a hardware interrupt **to ring
  3**. `irq.S` checks the saved CS (`test dword [esp+48], 3`): if returning to ring 0 it skips the
  reschedule entirely — the kernel is never preempted mid-instruction. When returning to ring 3 it
  calls `schedPreempt` → `Scheduler::preempt()`, which switches iff `g_needResched`. At that point
  the kernel stack holds a full, clean trap frame, so the switch is safe.
- **Voluntary switches** happen whenever kernel code calls `schedule()` directly — via
  `yield()`, `block()`, `sleepUntil()`, `sleepOn()`, `ioWait()`, or when a task body returns.

`schedule()` itself does the bookkeeping under a brief `cpuIrqSave`/`cpuIrqRestore` section (so a
concurrent `onTick` can't interleave on the shared `g_tasks` states): pick the next runnable task
(`pickNext`, round-robin, idle only when nothing else runs), flip the RUNNING/READY fields,
`setKernelStack(next->esp0)`, `setCurrent(next->proc)` — then **restore IF before** the actual
`archContextSwitch`, which touches no shared state and runs with the caller's original IF.

> Why deferred? A context switch mid-IRQ would either corrupt the interrupted task's kernel stack or
> require copying a partial frame. Deferring to the ring-3 return (or a voluntary yield) keeps every
> task's kernel stack a clean stack of complete frames. **Do not reintroduce switching inside the
> IRQ handler.**

---

## 3. The context switch (arch)

`archContextSwitch(unsigned* saveOldKesp, unsigned newKesp)` (`switch.S`): push the callee-saved
registers (`ebx/esi/edi/ebp`) **and CR3**, save `esp` into `*saveOldKesp`, load `newKesp`, then pop
— reloading CR3 (and flushing the TLB) **only if it actually changed**, so a kernel-thread ↔
kernel-thread switch (same address space) skips the flush. The whole context lives on the kernel
stack; `kesp` is the one word the `Task` stores.

A freshly created task has no real frame to return into, so `archTaskBootstrap` **fabricates** one:
it plants `(cr3, ebp=0, edi=0, esi=0, ebx=0, ret=taskTrampoline)` so the first switch "returns" into
`taskTrampoline`, which does `sti` (kernel threads run with interrupts on) and calls
`schedulerRunCurrentBody` → `Scheduler::runCurrentBody()` → the task `body()`. When a body returns,
the task is marked `TASK_DONE` and reaped lazily.

`fork` instead fabricates the child's stack from the **parent's syscall trap frame**
(`archForkChild`), so the first switch into the child `ret`s through `ret_from_fork` and `iret`s the
copied frame with `eax = 0` into ring 3 — see §5.

The **timer**: `archTimerInit(1000)` programs PIT channel 0 (divisor `1193180/1000 = 1193`) and
routes IRQ0 to `timerTick`, which calls `onTick((cs & 3) == 3)`. `Scheduler::ticks()` (the global
`g_ticks`) is the monotonic clock behind `nanosleep`, poll/select timeouts, the TCP timers, and
`/proc/uptime`.

---

## 4. Blocking & wakeups

Tasks block on **`WaitQueue`** (`kernel/WaitQueue.h`) — an intrusive singly-linked list over
`Task::waitNext`, pure data structure, no scheduler/arch dependency. The primitives:

| Primitive | Use |
|---|---|
| `sleepOn(q)` | park on an event queue until `wakeAll(q)` or a signal; unlink on return. |
| `sleepOnUntil(q, ready, ctx)` | loop, re-testing `ready(ctx)` under interrupts-off — closes the lost-wakeup window for an **IRQ-driven** waker (the keyboard filling the console buffer). |
| `sleepUntil(tick)` | block until a deadline; `onTick` wakes it. One wakeup for a long sleep, not one per tick. |
| `block()` | generic deschedule (job-control stop, legacy retry). |
| `ioWait()` | `sleepUntil(now+1)` — legacy I/O retry, re-test the condition next tick. |

Wakeups only **flag** a state change, never switch (IRQ-safe):

- `wake(t)` — `TASK_BLOCKED → TASK_READY` + `g_needResched`. Guarded to touch only BLOCKED tasks, so
  it can never resurrect a `TASK_ZOMBIE`/`TASK_DONE` into its final `for(;;)`.
- `resume(t)` — `TASK_STOPPED → TASK_READY`, kept distinct from `wake()` so an ordinary I/O wakeup
  or SIGCHLD can never un-stop a Ctrl+Z'd process — only SIGCONT/SIGKILL does.
- `wakeAll(q)` — ready every task parked on `q`; walks the list under `cpuIrqSave` with a slot-bound
  guard so a corrupt queue degrades to a no-op instead of faulting.

The lost-wakeup race is closed by doing the *enqueue + flip-to-BLOCKED* under `cpuIrqSave`: if an
IRQ waker fires in the gap before `schedule()`, it finds the task already READY and `schedule()`
simply repicks it. Every blocking primitive also checks `hasPendingSignalCurrent()` first, so a task
never sleeps past a signal already posted to it (the basis for EINTR / restart).

> This event-driven blocking is why kernel threads must **yield/block on a WaitQueue**, not spin.
> The net RX softirq (`ksoftirqd-net`) sleeps on a queue that `netifRx` wakes from the e1000 IRQ
> (networking.md §3). The only legitimate spin is the **idle task** (`idleBody`: `halt_or_hlt`
> = `sti; hlt`, then `schedule()`), which runs in ring 0 and so is never preempted — it must
> voluntarily yield to anything `onTick` just made runnable.

---

## 5. Process lifecycle

`Process` (`kernel/Process.h`) carries: `pid`, `parent`, `task`, `space` (`arch::AddressSpace*`),
`sys` (`Syscalls*` = per-process fd table + exit status), `exitCode`/`termSignal`, `kthread`,
`comm`/`cmdline`, the heap (`brkBase/brkCur/brkMax`) and `mmapNext` bump pointer, the job-control
ids `pgid`/`sid`, and the `SignalState`. `ProcTable` holds `g_procs[MAXPROC = 1024]`; pids come from
a monotonically increasing `g_nextPid` (**never reused**); `byPid` is a linear scan.

**Boot tasks** (`kernel/Kernel.cpp`): `Scheduler::init()` makes the idle task (slot 0);
`registerKthread` wraps a `Task` in a `Process` marked `kthread` (so it shows in `/proc` like a
Linux `[kworker]`); init is **pid 1**, runs `initTaskBody` which `execProgram`s
`/disks/main/nanos/core/init.nxe` into ring 3; the net core registers `ksoftirqd-net` and
`net-timer` as kthreads. Then `archTimerInit(1000)` and `Scheduler::start()` switch into the first
runnable task and never return.

- **`fork`** (`forkProcess`, `Exec.cpp`): allocate a child pid, **eagerly copy** the address space
  (`mmuCopyAddressSpace` — image, heap, module and mmap windows), dup the fd table, inherit
  `pgid`/`sid` and signal dispositions, `createBlank` a task, and `archForkChild` fabricates its
  kesp from the parent trap frame. Parent gets the child pid; child returns 0.
- **`execve`**: replace the image in place (same pid, fresh address space), `archFrameToUser`
  rewrites the trap frame so the syscall's `iret` enters the new program, reset caught signal
  handlers, close `FD_CLOEXEC` descriptors.
- **`exit`** (`procExit`): mark the task `TASK_ZOMBIE`, send `SIGCHLD` to the parent and wake it,
  re-parent any children to init, then `schedule()` away.
- **`waitpid`** (`waitProcess`): reap a zombie child — `Scheduler::reap` frees the 32 KiB stack,
  `delete sys`, free the proc slot — and return the encoded status; blocks (signal-interruptible)
  if no child is ready and `WNOHANG` isn't set; `-ECHILD` if there are none.

**Job control:** each process has `pgid`/`sid`; `setpgid`/`setsid` form groups and sessions. The
terminal tracks a foreground process group; Ctrl+C/Ctrl+Z on the PTY deliver SIGINT/SIGTSTP to that
whole group (`consoleSignalGroup`). `/proc/<pid>/…` is generated from the proc table.

---

## 6. Signals & scheduling

`SignalState` holds the `pending`/`blocked`/`restart` masks, the per-signal `handlers[]`, and the
`restorer` trampoline address. Signals are **posted** asynchronously (`sigPost` sets a pending bit)
but **delivered at the same deferred boundary as preemption** — the return to ring 3:
`signalDeliver(tf, origEax, inSyscall)` runs from the syscall return path (`syscall_x86.cpp`, after
the syscall, `inSyscall=true`) and from the hardware-interrupt return path (`Interrupt.cpp`,
`inSyscall=false`). It resolves each pending, unblocked signal (terminate / ignore / stop /
continue / run handler) and, for a handler, calls `archPushSignalFrame` to build the signal frame on
the user stack (see nxe-ndl.md §6.5).

Because delivery is deferred to the ring-3 boundary and never happens in IRQ context, a signal posted
from an interrupt is simply queued and delivered when the target next returns to user mode. A task
blocked in a WaitQueue is made runnable by the post (`wake`), then notices the pending signal on
return and either restarts the syscall (`SA_RESTART`) or returns `-EINTR`.

---

## 7. Invariants & status

- **Single CPU.** No SMP, no spinlocks, no per-CPU run queues. Mutual exclusion against the timer/
  IRQs is `cpuIrqSave`/`cpuIrqRestore` (push flags + `cli` / restore).
- **Interrupt state.** Syscalls run with IF=0 (`isr128` does `cli`); kernel threads run with IF=1
  (the trampoline `sti`). `schedule()` restores the caller's IF across the switch.
- **Preemption points are exactly two:** the ring-3 return from an IRQ, and a voluntary
  `schedule()`. Ring-0 code (including kernel threads between yields) is never involuntarily
  switched — kernel threads must cooperate by blocking on a WaitQueue or sleeping.
- **History.** The current scheduler is fully live (Stage 3+). The parked `arch/x86/cpu/
  MultiTasking.cpp` is the old, disabled EIP-only attempt — not used. (CLAUDE.md's "multitasking
  experimental, disabled" line predates this scheduler.)

### Key files

| Component | File |
|---|---|
| MI scheduler (table, states, policy, block/wake) | `kernel/Scheduler.{h,cpp}` |
| MI/MD contract | `arch/include/arch/sched.h` |
| context switch + trampoline | `arch/x86/cpu/switch.S` |
| bootstrap, timer, preempt bridge | `arch/x86/cpu/sched_x86.cpp` |
| deferred-preemption ring-3 gate | `arch/x86/cpu/irq.S` |
| wait lists | `kernel/WaitQueue.h` |
| process table | `kernel/Process.{h,cpp}` |
| fork / execve / exit / wait | `kernel/Exec.cpp` |
| signals + delivery | `kernel/Signal.{h,cpp}`, `signalDeliver` (Exec.cpp), `syscall_x86.cpp` / `Interrupt.cpp` |
| boot wiring (idle, init, kthreads) | `kernel/Kernel.cpp` |
