# Stage 3 — Preemptive Scheduler + Task Abstraction — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use `- [ ]`.

**Goal:** A preemptive round-robin scheduler over a `task` abstraction (per-task kernel stack, stack-pointer-swap context switch, idle task), so init/nsh and a kernel clock thread time-share the CPU; blocked console reads yield; `/proc/uptime` becomes real.

**Architecture:** MI `Scheduler` (task table, states, round-robin selection — host-tested) + arch `archContextSwitch`/`archTaskBootstrap`/`archTimerInit` (`switch.S` + `sched_x86.cpp`). Timer PIT @1000 Hz fires IRQ0 → `Scheduler::onTick()` → `schedule()` → `archContextSwitch`. Each task is switched out inside its own `schedule()` (Linux `switch_to` model). Only Task A is ring 3, so `g_userCtx`/`esp0` stay global.

**Spec:** `docs/superpowers/specs/2026-06-07-stage3-scheduler-design.md`
**Branch/commits:** `dockerized-build`, one commit per task, no Claude attribution.

---

## File structure
- `kernel/Scheduler.h` / `Scheduler.cpp` — MI: `Task`, task table, `nextRunnable` (pure), schedule/yield/block/wake/onTick, tick counter, `runCurrentBody`.
- `arch/include/arch/sched.h` — contract: `archContextSwitch`, `archTaskBootstrap`, `archTimerInit`.
- `arch/x86/cpu/switch.S` — `archContextSwitch` + `taskTrampoline` (nasm).
- `arch/x86/cpu/sched_x86.cpp` — `archTaskBootstrap`, `archTimerInit`, the IRQ0 tick handler.
- `tests/test_scheduler.cpp` — host test for `nextRunnable` + tick/uptime formatting.
- Modify: `kernel/Kernel.cpp` (boot wiring), `arch/x86/drivers/input_x86.cpp` (block/wake), `fs/SynthFs.cpp` (real uptime), `arch/x86/arch.mk` (drop `MultiTasking.o`, add `switch.o sched_x86.o`), `Makefile` (`Scheduler.o` in MI + TEST_MODULES/COV).
- Remove: `arch/x86/cpu/MultiTasking.cpp` / `.h`.

---

## Task 1: Scheduler MI core + round-robin selection (host-tested)

**Files:** Create `kernel/Scheduler.h`, `kernel/Scheduler.cpp`, `tests/test_scheduler.cpp`; modify `Makefile`.

- [ ] **Step 1 — failing test** `tests/test_scheduler.cpp`:

```cpp
#include "doctest.h"
#include "Scheduler.h"
using namespace kernel;

TEST_CASE("nextRunnable: round-robins non-idle tasks, idle (0) only as fallback") {
    TaskState st[3] = { TASK_READY, TASK_READY, TASK_READY };   // 0=idle,1=A,2=B
    CHECK(Scheduler::nextRunnable(st, 3, 1) == 2);   // A -> B
    CHECK(Scheduler::nextRunnable(st, 3, 2) == 1);   // B -> A (skip idle)
    CHECK(Scheduler::nextRunnable(st, 3, 0) == 1);   // idle -> A
}
TEST_CASE("nextRunnable: blocked tasks are skipped; idle when none runnable") {
    TaskState st[3] = { TASK_READY, TASK_BLOCKED, TASK_BLOCKED };
    CHECK(Scheduler::nextRunnable(st, 3, 1) == 0);   // all non-idle blocked -> idle
    TaskState st2[3] = { TASK_READY, TASK_READY, TASK_BLOCKED };
    CHECK(Scheduler::nextRunnable(st2, 3, 1) == 1);  // only A runnable -> stays A
}
TEST_CASE("nextRunnable: DONE tasks are skipped like blocked") {
    TaskState st[3] = { TASK_READY, TASK_DONE, TASK_READY };
    CHECK(Scheduler::nextRunnable(st, 3, 2) == 0);   // B done? no: cur=2(B) -> skip 1(done),0(idle) -> idle
}
```

- [ ] **Step 2 — run, confirm fail** (`make test` → `Scheduler.h` not found).

- [ ] **Step 3 — `kernel/Scheduler.h`**:

```cpp
#ifndef SCHEDULER_H_
#define SCHEDULER_H_
namespace kernel {

enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_DONE };

struct Task {
    unsigned kesp;        // saved kernel esp (whole context lives on the stack)
    TaskState state;
    void (*body)();
    int id;               // 0 = idle
    unsigned char* kstack;
};

class Scheduler {
public:
    static void init();                              // create the idle task (id 0)
    static Task* create(void (*body)(), int id);     // bootstrap a task, mark READY
    static void start();                             // switch into the first runnable
    static void schedule();                          // pick next + context switch
    static void onTick();                            // timer: ticks++ then schedule
    static void yield()  { schedule(); }
    static void block();                             // current -> BLOCKED, schedule
    static void wake(Task* t);                       // -> READY (IRQ-safe)
    static Task* current();
    static unsigned ticks();
    static void runCurrentBody();                    // called by the trampoline

    // Pure: index of the next task to run, given states. Idle (index 0) is only
    // chosen when no non-idle task is runnable. Host-tested.
    static int nextRunnable(const TaskState* st, int n, int cur);
};

}
#endif
```

- [ ] **Step 4 — `kernel/Scheduler.cpp`** (the pure fn + state; arch calls stubbed until Task 2):

```cpp
#include "Scheduler.h"
#include <arch/sched.h>

namespace kernel {

static const int MAXTASKS = 8;
static const int KSTACK_SIZE = 8192;
static Task g_tasks[MAXTASKS];
static unsigned char g_kstacks[MAXTASKS][KSTACK_SIZE] __attribute__((aligned(16)));
static int g_ntasks = 0;
static int g_cur = 0;
static volatile unsigned g_ticks = 0;

static bool runnable(TaskState s) { return s == TASK_READY || s == TASK_RUNNING; }

int Scheduler::nextRunnable(const TaskState* st, int n, int cur) {
    for (int i = 1; i <= n; i++) {
        int idx = (cur + i) % n;
        if (idx != 0 && runnable(st[idx]))
            return idx;
    }
    return 0;   // idle fallback
}

void Scheduler::init() {
    g_ntasks = 0;
    // idle task (id 0): hlt loop. Bootstrapped like any task.
    create(0, 0);          // body filled below to the idle loop
}

static void idleBody() { for (;;) arch::halt_or_hlt(); }   // see note

Task* Scheduler::create(void (*body)(), int id) {
    Task* t = &g_tasks[g_ntasks];
    t->id = id;
    t->body = body ? body : idleBody;
    t->state = TASK_READY;
    t->kstack = g_kstacks[g_ntasks];
    t->kesp = arch::archTaskBootstrap(t->kstack + KSTACK_SIZE, arch::archKernelCr3());
    g_ntasks++;
    return t;
}

Task* Scheduler::current() { return &g_tasks[g_cur]; }
unsigned Scheduler::ticks() { return g_ticks; }

void Scheduler::schedule() {
    TaskState st[MAXTASKS];
    for (int i = 0; i < g_ntasks; i++) st[i] = g_tasks[i].state;
    int next = nextRunnable(st, g_ntasks, g_cur);
    if (next == g_cur) return;                 // nothing else to run
    int prev = g_cur;
    g_cur = next;
    if (g_tasks[prev].state == TASK_RUNNING) g_tasks[prev].state = TASK_READY;
    g_tasks[next].state = TASK_RUNNING;
    arch::archContextSwitch(&g_tasks[prev].kesp, g_tasks[next].kesp);
}

void Scheduler::onTick() { g_ticks++; schedule(); }
void Scheduler::block()  { g_tasks[g_cur].state = TASK_BLOCKED; schedule(); }
void Scheduler::wake(Task* t) { if (t) t->state = TASK_READY; }

void Scheduler::start() {
    g_cur = 0;
    g_tasks[0].state = TASK_RUNNING;
    // Bootstrap into the idle task's context; but we actually want to run the
    // first non-idle task. Pick it and switch from a throwaway context.
    static unsigned throwaway;
    int first = nextRunnable(...);   // see Task 4 for the real start sequence
}

void Scheduler::runCurrentBody() {
    g_tasks[g_cur].body();
    g_tasks[g_cur].state = TASK_DONE;
    schedule();
    for (;;) {}   // unreachable
}

}
```

> NOTE: Step 4 is the host-testable skeleton; `nextRunnable` is the only part tests
> exercise. `start()`, `idleBody`, and the arch calls are finalized in Tasks 2/4 (the
> host build stubs `arch::*` in `tests/host_shims.cpp`). Keep `start()` minimal here;
> its real body is written in Task 4 once `archContextSwitch` exists.

- [ ] **Step 5 — host stubs** `tests/host_shims.cpp`: add `namespace arch { void archContextSwitch(unsigned*, unsigned){} unsigned archTaskBootstrap(unsigned char*, unsigned){return 0;} void archTimerInit(unsigned){} unsigned archKernelCr3(){return 0;} void halt_or_hlt(){} }` so the MI Scheduler links in tests.

- [ ] **Step 6 — Makefile**: `MI_SOURCES += Scheduler.o`; add `kernel/Scheduler.cpp` to `TEST_MODULES`; add `"*/Scheduler.*"` to `COV_PATTERNS`.

- [ ] **Step 7 — run** `make test` (nextRunnable cases pass).

- [ ] **Step 8 — commit** `feat: scheduler core + round-robin task selection (host-tested)`.

---

## Task 2: arch context switch + bootstrap (switch.S, sched_x86)

**Files:** Create `arch/include/arch/sched.h`, `arch/x86/cpu/switch.S`, `arch/x86/cpu/sched_x86.cpp`; modify `arch/x86/arch.mk`.

- [ ] **Step 1 — `arch/include/arch/sched.h`**:

```cpp
#pragma once
namespace arch {
extern "C" void archContextSwitch(unsigned* saveOldKesp, unsigned newKesp);
unsigned archTaskBootstrap(unsigned char* kstackTop, unsigned cr3);
unsigned archKernelCr3();
void archTimerInit(unsigned hz);
void halt_or_hlt();   // idle primitive (sti; hlt)
}
```

- [ ] **Step 2 — `arch/x86/cpu/switch.S`** (nasm; saves callee-saved + CR3 + esp):

```asm
[BITS 32]
[GLOBAL archContextSwitch]
[GLOBAL taskTrampoline]
[EXTERN schedulerRunCurrentBody]

; void archContextSwitch(unsigned* saveOldKesp /*+4*/, unsigned newKesp /*+8*/)
archContextSwitch:
    push ebx
    push esi
    push edi
    push ebp
    mov eax, cr3
    push eax                 ; layout (top->): cr3,ebp,edi,esi,ebx,ret,arg1(+24),arg2(+28)
    mov eax, [esp + 24]      ; saveOldKesp
    mov [eax], esp           ; *saveOldKesp = esp
    mov esp, [esp + 28]      ; esp = newKesp  (arg2 read from old stack, then esp set)
    pop eax
    mov cr3, eax             ; restore address space (TLB flush)
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

; First entry of a freshly bootstrapped task.
taskTrampoline:
    sti                      ; allow preemption
    call schedulerRunCurrentBody
.hang:
    hlt
    jmp .hang
```

- [ ] **Step 3 — `arch/x86/cpu/sched_x86.cpp`**:

```cpp
#include <arch/sched.h>
#include "Interrupt.h"
#include "IOPort.h"
#include "PagingControl.h"   // readCr3
#include "Scheduler.h"

extern "C" void taskTrampoline();
extern "C" void schedulerRunCurrentBody() { kernel::Scheduler::runCurrentBody(); }

namespace arch {

unsigned archKernelCr3() { return kernel::readCr3(); }   // kernel dir at boot time

void halt_or_hlt() { __asm__ __volatile__("sti; hlt"); }

unsigned archTaskBootstrap(unsigned char* kstackTop, unsigned cr3) {
    unsigned* sp = (unsigned*) kstackTop;
    *--sp = (unsigned) taskTrampoline;   // ret target of the first switch
    *--sp = 0;                           // ebp
    *--sp = 0;                           // edi
    *--sp = 0;                           // esi
    *--sp = 0;                           // ebx
    *--sp = cr3;                         // cr3 (popped first by archContextSwitch)
    return (unsigned) sp;
}

static void timerTick(kernel::Registers*) { kernel::Scheduler::onTick(); }

void archTimerInit(unsigned hz) {
    unsigned divisor = 1193180u / hz;            // 1000 Hz -> 1193
    kernel::IOPort::outb(0x43, 0x36);
    kernel::IOPort::outb(0x40, (unsigned char) (divisor & 0xFF));
    kernel::IOPort::outb(0x40, (unsigned char) ((divisor >> 8) & 0xFF));
    kernel::Interrupt::registerInterruptHandler(IRQ0, &timerTick);
}

}  // namespace arch
```

- [ ] **Step 4 — finalize `Scheduler::start()`** in `Scheduler.cpp` (now arch exists):

```cpp
void Scheduler::start() {
    // Switch from the (throwaway) boot context into the first non-idle task. The
    // boot stack is abandoned; control never returns here.
    TaskState st[MAXTASKS];
    for (int i = 0; i < g_ntasks; i++) st[i] = g_tasks[i].state;
    int first = nextRunnable(st, g_ntasks, 0);
    g_cur = first;
    g_tasks[first].state = TASK_RUNNING;
    static unsigned throwaway;
    arch::archContextSwitch(&throwaway, g_tasks[first].kesp);
}
```
Replace the placeholder `idleBody`/`halt_or_hlt` note: `idleBody` calls `arch::halt_or_hlt()` in a loop.

- [ ] **Step 5 — arch.mk**: in `ARCH_SOURCES`, drop `MultiTasking.o`, add `switch.o sched_x86.o`. (Leave `MultiTasking.cpp` deletion to Task 4.)

- [ ] **Step 6 — throwaway cooperative test (QEMU, temporary)**: in `Kernel::start`, before the existing exec, temporarily: `Scheduler::init(); Scheduler::create(threadA,1); Scheduler::create(threadB,2); Scheduler::start();` where threadA/threadB each loop `Console::write('A'/'B'); for-delay; Scheduler::yield();`. Build + boot headless; screen should show interleaved `ABABAB`. Then revert this temporary block. *Verify the context switch + bootstrap before adding preemption.*

- [ ] **Step 7 — commit** `feat: arch context switch + task bootstrap (switch.S)`.

---

## Task 3: preemption via PIT IRQ0

**Files:** modify `kernel/Kernel.cpp` (call `archTimerInit`), verify IRQ0 unmasked.

- [ ] **Step 1 — temporary preemption test (QEMU)**: same two threads as Task 2 Step 6 but WITHOUT the `yield()` calls (each just loops `Console::write` + a long delay). Add `arch::archTimerInit(1000);` before `Scheduler::start()`. Boot: the timer must preempt the busy loops → still interleaved `ABAB`. If only `A` prints, IRQ0 is masked — unmask it in the PIC (clear bit 0 of port 0x21) in `Idt`/PIC setup.
- [ ] **Step 2 — confirm** with `-d int`: periodic `v=20` (IRQ0) and interleaving. Revert the temporary block.
- [ ] **Step 3 — commit** `feat: preemptive scheduling on the 1000 Hz PIT timer`.

---

## Task 4: boot wiring (idle + init/nsh + clock), remove MultiTasking

**Files:** modify `kernel/Kernel.cpp`; remove `arch/x86/cpu/MultiTasking.cpp` + `.h`; `arch.mk` already updated.

- [ ] **Step 1 — task bodies + wiring** in `Kernel.cpp`. Replace the direct `execProgram(...)` block with:

```cpp
static Vfs* g_vfs;   // captured for the init task body
static volatile unsigned g_bgwork;

static void initTaskBody() {
    int rc = execProgram(g_vfs, "/disks/main/nanos/core/init.nxe");
    Console::write("init exited with code "); Console::writeLine(rc);
}
static void clockTaskBody() { for (;;) { g_bgwork++; arch::halt_or_hlt(); } }
```
and in `Kernel::start`, after `installSyscalls(vfs); arch::syscallSelfTest();`:
```cpp
    g_vfs = vfs;
    Scheduler::init();                       // idle = task 0
    Scheduler::create(initTaskBody, 1);      // init/nsh (enters ring 3)
    Scheduler::create(clockTaskBody, 2);     // background clock thread
    arch::archTimerInit(1000);
    Scheduler::start();                      // never returns
```
Remove the old `for(run...)`/`execProgram` lines and the commented `MultiTasking` block.

- [ ] **Step 2 — remove** `arch/x86/cpu/MultiTasking.cpp` and `MultiTasking.h` (`git rm`). Confirm `arch.mk` no longer lists `MultiTasking.o`.

- [ ] **Step 3 — build + check-arch + test** all green.

- [ ] **Step 4 — QEMU**: boots to `nsh$`; `ls`, `cat`, `echo` still work; no faults. (Clock thread runs but is silent.)

- [ ] **Step 5 — commit** `feat: boot into the scheduler (idle + init/nsh + clock); retire MultiTasking`.

---

## Task 5: block/wake on console input (stop hlt-spinning)

**Files:** modify `arch/x86/drivers/input_x86.cpp`.

- [ ] **Step 1 — block on empty read, wake on data** in `input_x86.cpp`. Add a single waiter:

```cpp
#include "Scheduler.h"
namespace { kernel::Task* g_inputWaiter = 0; }

// in inputRead, replace the `while(!ready){ sti; hlt; }` spin loops with:
//   while (!available) { g_inputWaiter = kernel::Scheduler::current(); kernel::Scheduler::block(); }
//   g_inputWaiter = 0;
// (block() deschedules until woken; on resume re-check availability.)

// in inputFeedScancode, after pushing data that makes a read satisfiable
// (cooked: g_line.lineReady(); raw: ring non-empty), wake the waiter:
//   if (g_inputWaiter) { kernel::Scheduler::wake(g_inputWaiter); }
```
Keep a fallback `arch::halt_or_hlt()` if `g_inputWaiter` can't be set (pre-scheduler). Guard `Scheduler::current()` validity.

- [ ] **Step 2 — build + test + QEMU**: nsh idle no longer busy-spins (the clock thread / idle get the CPU); typing still works; `ls`/`cat` work. No faults.

- [ ] **Step 3 — commit** `feat: blocking console read yields to the scheduler (block/wake)`.

---

## Task 6: real /proc/uptime from the tick counter

**Files:** modify `fs/SynthFs.cpp`; extend `tests/test_synthfs.cpp` (or `test_scheduler.cpp`).

- [ ] **Step 1 — failing test**: a tiny pure formatter `Scheduler` exposes `ticks()`; test that the uptime generator renders a number. Since the generator reads live ticks, test a helper `int fmtUptime(char* buf, unsigned ticks, unsigned hz)` (extract it) → e.g. ticks=2500,hz=1000 → "2 s\n" or "2500 ticks\n". Assert output.

- [ ] **Step 2 — implement** in `SynthFs.cpp`: replace `gen_uptime`'s fixed string with one that reads `kernel::Scheduler::ticks()` and renders via `fmtUptime` into a static buffer, served by offset (so `cat` terminates). Include `Scheduler.h`.

- [ ] **Step 3 — run host test** (`make test`).

- [ ] **Step 4 — QEMU**: `cat /proc/uptime` twice (with a pause) shows an increasing value → proves the timer + scheduler ran. No faults.

- [ ] **Step 5 — commit** `feat: real /proc/uptime from the scheduler tick counter`.

- [ ] **Step 6 — update** `docs/superpowers/ROADMAP.md` (Stage 3 ✅).

---

## Self-review
- **Spec coverage:** Task abstraction+states (T1), context switch+bootstrap (T2), PIT preemption (T3), idle+init+clock boot / remove MultiTasking (T4), block/wake (T5), real uptime (T6). All spec sections mapped.
- **CR3 live save:** handled inside `archContextSwitch` (reads `cr3` at switch-out). ✓
- **esp0 untouched** by scheduler (only Task A ring 3) — no change needed; documented. ✓
- **Bootstrap layout** matches `archContextSwitch`'s pop order (cr3,ebp,edi,esi,ebx,ret). ✓
- **Idle never starves others:** `nextRunnable` returns idle only as fallback (host-tested). ✓
- **No placeholders** except the explicitly-deferred `start()` body, finalized in Task 2 Step 4 (noted, with code). 
- **Risk:** IRQ0 may be masked → Task 3 Step 1 covers unmasking. Bootstrap stack layout is the triple-fault risk → Task 2 Step 6 verifies cooperatively before preemption.
```
