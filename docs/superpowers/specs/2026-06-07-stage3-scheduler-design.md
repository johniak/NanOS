# NanOS — Stage 3: preemptive scheduler + `task` abstraction

_Design spec. Date: 2026-06-07. Branch: `dockerized-build`. Roadmap: Stage 3._

## Goal

Add a **preemptive round-robin scheduler** over a `task` abstraction, so several
tasks coexist and time-share the CPU. This is the Linux-style core (per-task kernel
stack, stack-pointer-swap context switch, kernel threads borrowing the kernel
address space, an idle task, init as the first task). `fork`/`exec`/`wait` (userland
creating tasks) stay in **Stage 4**; here the kernel creates the initial tasks.

Faithful to Linux in mechanism; simplified in policy:
- **Round-robin**, not CFS.
- A **timer tick** preempts (the "safety net"), **plus voluntary yield/block** when a
  task waits on I/O (the common case in Linux). Reschedule happens directly in the
  timer IRQ handler (Linux defers via `need_resched`; we keep it simple — our kernel
  is single-CPU and has few interruptible critical sections).
- Tasks are created by the kernel, not via `fork`/`clone` (Stage 4).

## Non-goals (Stage 3)

- `fork`/`exec`/`wait`, zombies, per-task fd-table copies (Stage 4).
- More than one ring-3 task. Only Task A (init→nsh) runs in ring 3; the rest are
  kernel threads. This keeps the single global ring-3 context (`g_userCtx`) and the
  existing `esp0`/nested-spawn machinery unchanged.
- SMP / per-CPU runqueues, priorities, deferred (`need_resched`) reschedule, CFS.

## Architecture

```
kernel/Scheduler.{h,cpp}        MI: task list, states, round-robin, yield/block/wake
arch/include/arch/sched.h       MI/MD contract: archContextSwitch, archTaskBootstrap,
                                archTimerInit
arch/x86/cpu/switch.S           asm: save/restore callee-saved + esp (+ cr3 helper)
arch/x86/cpu/sched_x86.cpp      implements <arch/sched.h>: bootstrap a kernel stack,
                                CR3 save/restore, PIT IRQ0 setup -> scheduler tick
```

### Task

```cpp
enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED };

struct Task {
    unsigned kesp;        // saved kernel stack pointer (the whole context lives here)
    unsigned cr3;         // address space at the last switch-out (live value)
    TaskState state;
    unsigned char* kstack;// this task's kernel stack (KSTACK_SIZE)
    void (*body)();       // entry; bootstrapped on first run
    int id;               // 0 = idle, 1 = init/nsh, ...
};
```

From the scheduler's view every task is a "kernel thread": a kernel stack + a body
function. Task A's body happens to enter ring 3 via the existing `execProgram` /
`enterUser` — that ring transition is orthogonal to scheduling.

### Scheduler (MI, host-testable core)

- A small fixed array of `Task*` + a current index. `schedule()` picks the next
  `TASK_READY`/`TASK_RUNNING` task round-robin; if none is runnable, it runs the
  **idle task** (id 0, body = `hlt` loop).
- `yield()` → `schedule()` (voluntary, stays READY).
- `block()` → set current `TASK_BLOCKED`, `schedule()` away.
- `wake(Task*)` → set `TASK_READY` (safe to call from IRQ context).
- `currentTask()`.
- The pure list/state/round-robin selection logic (which task runs next given states)
  is extracted so it is **host-tested** without any asm.

### Context switch (arch)

`archContextSwitch(unsigned* saveOldKesp, unsigned newKesp)` (`switch.S`):
- push callee-saved regs (ebx, esi, edi, ebp) on the current stack,
- save CR3 and esp into the old task (CR3 read **live** so a task preempted while
  running in another address space — e.g. nsh mid-`spawn` in the child's space —
  resumes with the right CR3),
- load the new task's CR3 + esp, pop its callee-saved regs, `ret`.

Because each task is switched out *inside its own* `schedule()` call, the new task
resumes inside *its* `schedule()` and unwinds back to whatever it was doing (a timer
IRQ frame → `iret`, or a blocked `read`, or the idle `hlt`). This is exactly Linux's
`switch_to` model.

**Bootstrap** (`archTaskBootstrap`): fabricate a fresh task's kernel stack so the
first `archContextSwitch` into it "returns" into a trampoline that calls `body()`.
(Linux's `ret_from_fork` equivalent.)

### Preemption — timer (the bell)

- `archTimerInit(hz)`: program the PIT (divisor `1193180/hz`) at **1000 Hz** (every
  1 ms, like a modern Linux desktop; divisor `1193`), register an IRQ0 handler. (The
  divisor math already exists in the parked `MultiTasking.cpp`.)
- The IRQ0 handler sends EOI (the existing `irq_handler` already does), increments a
  global **tick counter**, then calls `schedule()`. The switch happens through
  `archContextSwitch`; the existing `irq_common_stub` (`popad; iret`) resumes whichever
  task we land on.

### Voluntary yield / block on I/O (the common case)

- The console read (`arch::inputRead`) currently spins on `sti; hlt`. New behaviour:
  if no data is available, register the current task as the **single console waiter**
  and `block()`; when `inputFeedScancode` produces a ready line (cooked) or a byte
  (raw), it `wake()`s that waiter. The reader rechecks on wake.
- This means a blocked shell consumes no CPU; the clock thread (and idle) run instead.

### `/proc/uptime` becomes real

The tick counter (incremented every timer IRQ) is exposed: the SynthFs `/proc/uptime`
generator renders ticks (and/or seconds = ticks/hz) instead of the fixed placeholder.
This is the visible proof the timer + scheduler run.

### Boot wiring (`Kernel::start`)

After the storage/syscall setup, instead of calling `execProgram(init)` directly on
the boot stack:
1. `schedulerInit()` — create the **idle task** (id 0).
2. Create **Task A** (id 1), body = `runInit()` (calls `execProgram("/disks/main/
   nanos/core/init.nxe")`).
3. Create **Task B** (id 2), body = the clock thread (loop: increment a work counter;
   `yield()` periodically) — demonstrates a second runnable task.
4. `archTimerInit(1000)`.   /* 1000 Hz — 1 ms tick */
5. `schedulerStart()` — `archContextSwitch` into Task A and never return (the boot
   stack is abandoned; the idle task takes over when nothing else is runnable).

`esp0` is **not** touched by the scheduler: only Task A is ring-3, so its `esp0`
(`g_kstacks[depth]`, set by the existing usermode/spawn path) persists across switches
to the ring-0 tasks and back.

## Data flow (proof of concurrency)

`cat /proc/uptime` (run twice, with nsh idle on `read` in between) shows the tick
count increased → the timer fired and the scheduler ran other tasks while nsh was
blocked. The clock thread's work counter increasing proves Task B got CPU (not just
the timer handler).

## Error handling / safety

- `wake()` from IRQ context only flips a state field (no allocation, no switch) — safe.
- If the idle task is the only runnable task, the system idles in `hlt` until the next
  IRQ (timer or keyboard) — correct.
- A task body that returns (e.g. init/nsh exits) marks the task done and `schedule()`s
  away permanently (its slot is freed/skipped).
- The single-console-waiter assumption holds because only Task A reads the console; a
  general wait-queue is deferred.

## Testing

Host-testable (MI, pure logic — no asm):
- `Scheduler` round-robin selection: with tasks in mixed READY/BLOCKED states, `next()`
  picks the right one; all-blocked → idle; block/wake transitions; a finished task is
  skipped.
- The tick→uptime rendering (ticks/hz formatting) in the `/proc/uptime` generator.

QEMU (the asm switch, bootstrap, PIT, preemption are hardware — verified live):
- boots into `nsh$`; `cat /proc/uptime` twice shows an increasing value; nsh stays
  interactive (type while ticks advance); `ls`/`cat` still work; no `v=08/0d/0e` faults.
- `-d int` shows periodic `v=20` (IRQ0) and that execution resumes correctly across
  switches.

## Risks / gotchas

- **CR3 must be saved live** at switch-out (not from a fixed task field), because Task
  A's active CR3 changes during nested `spawn` (nsh space → child space → back).
- **First switch / bootstrap** must fabricate the stack exactly so `archContextSwitch`'s
  `ret` lands in the trampoline; getting the pushed layout wrong → triple fault.
- **Reschedule inside the timer IRQ**: the new task we land on must itself have been
  switched out at a point that can resume (another timer frame, a blocked read, or
  bootstrap). All paths go through `archContextSwitch`, so this holds.
- **EOI ordering**: `irq_handler` sends EOI before `schedule()`; each tick sends exactly
  one EOI on its own task's path. No double-EOI.
- **Only one ring-3 task** — relied upon so `g_userCtx`/`esp0` stay global. Adding a
  second ring-3 task (Stage 4 fork) requires per-task `g_userCtx`/`esp0`.
- `mmuDestroyAddressSpace` still leaks (pre-existing) — unchanged here.

## Files

- Create: `kernel/Scheduler.h`, `kernel/Scheduler.cpp`, `arch/include/arch/sched.h`,
  `arch/x86/cpu/switch.S`, `arch/x86/cpu/sched_x86.cpp`, `tests/test_scheduler.cpp`.
- Modify: `kernel/Kernel.cpp` (boot wiring), `arch/x86/drivers/input_x86.cpp`
  (block/wake on read), `fs/SynthFs.cpp` (real `/proc/uptime`), `arch/x86/arch.mk`
  (new objects), `Makefile` (`Scheduler.o` in MI + TEST_MODULES/COV), `kernel/SyscallNr.h`
  if a `yield` syscall is wanted (not required for Stage 3).
- Remove/replace: `arch/x86/cpu/MultiTasking.{cpp,h}` (broken EIP-only attempt).

## Build order (one commit each; build + test + check-arch green)

1. `Scheduler` MI core + states + round-robin selection + host tests (no asm yet;
   `archContextSwitch` stubbed/not called).
2. `switch.S` + `arch/sched.h` + `sched_x86.cpp` bootstrap & context switch; a throwaway
   kernel-only test: two kernel threads cooperatively `yield()` back and forth (QEMU).
3. PIT IRQ0 + tick counter + preemptive `schedule()`; verify two kernel threads
   preempt without explicit yield (QEMU, `-d int` shows v=20).
4. Boot wiring: idle + Task A (init/nsh) + Task B (clock); replace the direct
   `execProgram` call; remove `MultiTasking`. QEMU: boots to `nsh$`, stays interactive.
5. Block/wake: `inputRead` blocks the task and the keyboard feed wakes it (no more
   `hlt`-spin). QEMU: nsh idle uses no CPU; clock advances.
6. Real `/proc/uptime` from the tick counter (+ host test for the formatting). QEMU:
   `cat /proc/uptime` increases.
```
