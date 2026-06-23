#include "Scheduler.h"
#include "Process.h"
#include "memory_manager.h"   // malloc/free: kernel stacks are heap-allocated per task
#include <arch/sched.h>
#include <arch/cpu.h>         // cpuIrqSave/Restore: protect the schedule() state mutation
#include "WaitQueue.h"        // sleepOn/wakeAll operate on these event lists
#include "SignalDispatch.h"   // hasPendingSignalCurrent: don't sleep through a pending signal
#include "Bkl.h"              // Big Kernel Lock: released across the context switch (SMP)

namespace kernel {

// One scheduler task per process (kernel threads included) plus a little slack. The task
// SLOTS are cheap (a handful of words each); the expensive part — each task's 8 KB kernel
// stack — is allocated from the heap on create and freed on reap, so the live task count is
// bounded by RAM, not by a static array (which at this ceiling would be tens of MB).
static const int MAXTASKS = ProcTable::MAX + 8;
// 32 KiB per-task kernel stack. The stacks are heap-allocated, so an overflow scribbles the
// adjacent heap block (the heap's boundary-tag/canary check, mm/Heap.cpp, now turns that into a
// clean panic instead of silent corruption). The deep cost is the ext write + JBD2 journal path:
// several extent-tree helpers each hold a block-sized scratch buffer (char buf[4096]) and nest a
// few levels, and a keyboard IRQ can land on top — which overran 8 and even 16 KiB. (The longer-
// term fix is to take those block buffers off the stack; this sizing + the canary cover it now.)
static const int KSTACK_SIZE = 32768;

static Task g_tasks[MAXTASKS];
static int g_ntasks = 0;
static int g_cur = 0;
static volatile unsigned g_ticks = 0;
static unsigned g_ctxt = 0;   // total context switches performed (for /proc/stat ctxt)
static unsigned g_load[3] = { 0, 0, 0 };   // 1/5/15-min load, fixed-point FSHIFT=11

// Linux load-average decay (FSHIFT=11, FIXED_1=2048; the 5-second EXP_1/5/15 constants).
void loadDecay(unsigned load[3], int runnable) {
	static const unsigned EXP[3] = { 1884, 2014, 2037 };   // exp(-5s/{60,300,900}s) in FIXED_1
	unsigned n = (unsigned) (runnable < 0 ? 0 : runnable) << 11;   // runnable * FIXED_1
	for (int i = 0; i < 3; i++)
		load[i] = (load[i] * EXP[i] + n * (2048u - EXP[i])) >> 11;
}
static volatile bool g_needResched = false;   // a tick asked for a reschedule (deferred)

// Time-slice: a RUNNING task keeps the CPU for this many timer ticks (ms) before the tick
// forces a reschedule, instead of round-robining every single millisecond.
static const unsigned QUANTUM = 10;
static unsigned g_slice = 0;                   // ticks the current task has run on its slice

static bool runnable(TaskState s) { return s == TASK_READY || s == TASK_RUNNING; }

// Wrap-safe "has this timed wakeup come due?" (wakeAt 0 means the task has no timer armed).
bool Scheduler::timedWakeReady(unsigned now, unsigned wakeAt) {
	return wakeAt != 0 && (int) (now - wakeAt) >= 0;
}

// A reschedule is due when the running task has used up its quantum, or a sleeper just woke
// (so an I/O completion / expired timer preempts a CPU-bound task promptly rather than waiting
// out its whole slice).
bool Scheduler::shouldResched(unsigned sliceTicks, unsigned quantum, bool wokeSleeper) {
	return wokeSleeper || sliceTicks >= quantum;
}

// The idle task: sleep until an interrupt, then reschedule. The timer tick wakes I/O
// waiters (marking them READY) but, since idle runs in ring 0, never preempts here -- so
// idle must voluntarily yield to anything that became runnable.
static void idleBody() {
	for (;;) {
		// Drop the BKL so other CPUs (and an IRQ waker landing on this idle CPU) can run while
		// we halt; re-take it before touching scheduler state. The idle task runs in ring 0, so
		// the timer IRQ that wakes us does NOT preempt here (irq64.S skips schedPreempt for ring-0
		// interruptees) — it only flags g_needResched, which the schedule() below acts on.
		g_bkl.exit();
		arch::halt_or_hlt();
		g_bkl.enter();
		Scheduler::schedule();
	}
}

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
	g_cur = 0;
	g_ticks = 0;
	create(idleBody, 0);
}

// Find a reusable (FREE) slot below the high-water mark, or extend by one if there
// is room. Slots are recycled by reap() so fork/exec/exit churn does not exhaust the
// table. Returns -1 when the table is full.
static int findFreeSlot() {
	for (int i = 0; i < g_ntasks; i++)
		if (g_tasks[i].state == TASK_FREE)
			return i;
	return (g_ntasks < MAXTASKS) ? g_ntasks++ : -1;
}

static Task* allocSlot(int id) {
	unsigned char* stk = (unsigned char*) malloc(KSTACK_SIZE);   // per-task kernel stack (heap)
	if (!stk)
		return 0;                                                // out of memory -> fork -EAGAIN
	int i = findFreeSlot();
	if (i < 0) { free(stk); return 0; }                          // task table full
	Task* t = &g_tasks[i];
	t->id = id;
	t->body = 0;
	t->state = TASK_BLOCKED;   // not runnable until the caller has fabricated a valid kesp
	t->wakeAt = 0;
	t->waitNext = 0;
	t->kstack = stk;
	t->proc = 0;               // set when a Process binds this task (Kernel/forkProcess)
	t->esp0 = ((uintptr_t) (stk + KSTACK_SIZE)) & ~(uintptr_t) 15;   // 16-aligned TSS.esp0
	return t;
}

Task* Scheduler::create(void (*body)(), int id) {
	Task* t = allocSlot(id);
	if (!t)
		return 0;
	t->body = body ? body : idleBody;
	t->kesp = arch::archTaskBootstrap((unsigned char*) t->esp0, arch::archKernelCr3());
	t->state = TASK_READY;   // kesp is now valid -> the task may be scheduled (see allocSlot)
	return t;
}

// A bare task: slot + kernel stack + esp0, but no first-run trampoline. The caller
// fabricates `kesp` itself (fork plants a copied trap frame; see arch::archForkChild).
Task* Scheduler::createBlank(int id) {
	return allocSlot(id);
}

Task* Scheduler::current() { return &g_tasks[g_cur]; }
Task* Scheduler::idle() { return &g_tasks[0]; }   // idle is always the first task
unsigned Scheduler::ticks() { return g_ticks; }

// Same round-robin policy as nextRunnable (which stays the host-tested reference), but
// scanning the live task table in place: at this task ceiling a TaskState[MAXTASKS] copy
// would overflow the fixed 8 KB kernel stack, so never materialise one.
static int pickNext(int cur) {
	if (g_ntasks <= 0)
		return 0;
	for (int k = 1; k <= g_ntasks; k++) {
		int idx = (cur + k) % g_ntasks;
		if (idx != 0 && runnable(g_tasks[idx].state))
			return idx;
	}
	return 0;                                     // idle (slot 0) only when nothing else runs
}

void Scheduler::schedule() {
	// Pick the next task and flip the run-state fields under a brief interrupts-off section, so
	// a timer IRQ (onTick scans/mutates the same g_tasks states) can't interleave here. The flags
	// are restored BEFORE the context switch — the switch itself touches no shared state and runs
	// with the caller's original IF (kthreads with IF=1, syscalls with IF=0), exactly as before.
	unsigned long flags = arch::cpuIrqSave();
	int next = pickNext(g_cur);
	if (next == g_cur) {
		arch::cpuIrqRestore(flags);
		return;                                   // nothing else to run
	}
	g_ctxt++;                                     // an actual context switch (for /proc/stat)
	g_slice = 0;                                  // the newly-scheduled task gets a fresh quantum
	int prev = g_cur;
	g_cur = next;
	if (g_tasks[prev].state == TASK_RUNNING)
		g_tasks[prev].state = TASK_READY;
	g_tasks[next].state = TASK_RUNNING;
	arch::setKernelStack(g_tasks[next].esp0);   // ring3 traps land on next's kstack
	ProcTable::setCurrent(g_tasks[next].proc);  // route syscalls to it (O(1) back-pointer)
	ProcTable::setCurrentThread(g_tasks[next].thread);  // and the specific thread within it
	// Re-point the TLS descriptor at the now-current thread's TLS block, so __thread accesses
	// (%gs:-relative) resolve to the right per-thread storage. 0 = the thread has no TLS yet.
	arch::archLoadThreadTls(g_tasks[next].thread ? g_tasks[next].thread->tlsBase : 0);
	arch::cpuIrqRestore(flags);
	// BKL handoff: drop the lock so another CPU can enter the kernel while we switch, then
	// re-acquire on the far side. schedule() is always reached at BKL depth 1 (syscalls and
	// kernel threads enter at depth 1; schedPreempt only runs for ring-3 interruptees, also
	// depth 1), so a single exit() fully releases it and the resumed task's enter() restores
	// it. The switch itself touches only the two tasks' saved stacks — no shared state.
	g_bkl.exit();
	arch::archContextSwitch(&g_tasks[prev].kesp, g_tasks[next].kesp);
	g_bkl.enter();
}

// Timer tick. Does NOT switch tasks itself: it only advances the clock, re-wakes the I/O
// retry waiters, and flags that a reschedule is due. The actual context switch happens at
// the next safe point -- on the return path to ring 3 (preempt()) for a preempted user
// task, or at a voluntary yield/block in the kernel. This is the deferred-preemption model
// (à la Linux ret_from_intr): the kernel is never switched out at an arbitrary ring-0
// instruction, only at well-defined points, which keeps interrupt + syscall frames from
// interleaving on a task's kernel stack.
void Scheduler::onTick(bool fromUser) {
	g_ticks++;
	// Attribute this tick to the running process (user vs system by the ring it interrupted),
	// or to idle when the idle task (slot 0) was running.
	ProcTable::accountTick(fromUser, g_cur == 0);
	// Advance the per-process ITIMER_REAL timers by one tick of real time (the timer is
	// 1000 Hz => 1000 us/tick); expiring ones get SIGALRM posted + their threads woken.
	ProcTable::tickRealTimers(1000);
	// Sample the load average every 5 s (the timer is 1000 Hz). Runnable = non-idle tasks
	// in READY/RUNNING (slot 0 is idle).
	if (g_ticks % 5000u == 0) {
		int run = 0;
		for (int i = 1; i < g_ntasks; i++)
			if (runnable(g_tasks[i].state))
				run++;
		loadDecay(g_load, run);
	}
	bool woke = false;
	for (int i = 0; i < g_ntasks; i++) {
		if (g_tasks[i].state == TASK_BLOCKED && timedWakeReady(g_ticks, g_tasks[i].wakeAt)) {
			g_tasks[i].wakeAt = 0;            // timed wakeup due (I/O retry tick or sleep deadline)
			g_tasks[i].state = TASK_READY;
			woke = true;
		}
		// A kernel thread whose body() returned is left TASK_DONE and never waited on; reclaim
		// its slot + 8 KB stack lazily here (only when it is not the running task).
		else if (g_tasks[i].state == TASK_DONE && i != g_cur)
			reap(&g_tasks[i]);
	}
	// Only force a reschedule on a quantum boundary or when a sleeper woke — NOT every tick, so
	// two CPU-bound tasks no longer trade the CPU (and flush the TLB) 1000 times a second.
	if (shouldResched(++g_slice, QUANTUM, woke)) {
		g_slice = 0;
		g_needResched = true;
	}
}

unsigned Scheduler::contextSwitches() { return g_ctxt; }

void Scheduler::loadAvg(unsigned out[3]) {
	for (int i = 0; i < 3; i++)
		out[i] = (g_load[i] * 100u) >> 11;   // fixed-point -> hundredths
}

// Called on the return path from an interrupt to ring 3 (see irq.S): the only place a
// user task is involuntarily preempted, with a full, clean trap frame on its kernel stack.
void Scheduler::preempt() {
	if (g_needResched) {
		g_needResched = false;
		schedule();
	}
}

// Block in an I/O retry loop (empty pipe/pty, poll, nanosleep): deschedule and ask the
// timer tick to re-wake us, so we re-test our condition ~every ms instead of busy-spinning.
void Scheduler::ioWait() {
	sleepUntil(g_ticks + 1);   // legacy I/O retry: re-test the condition on the next tick
}

// Block until the timer reaches `tick` (a deadline), or until something wake()s us earlier
// (a signal). Used by nanosleep so a long sleep costs ONE wakeup at the deadline, not one per
// tick. The caller re-checks its condition on return (the wakeup may be a signal, not the timer).
void Scheduler::sleepUntil(unsigned tick) {
	unsigned long f = arch::cpuIrqSave();
	if (hasPendingSignalCurrent()) { arch::cpuIrqRestore(f); return; }   // don't sleep past a signal
	g_tasks[g_cur].wakeAt = tick ? tick : 1;   // 0 is the "no timer armed" sentinel
	g_tasks[g_cur].state = TASK_BLOCKED;
	arch::cpuIrqRestore(f);
	schedule();
}

void Scheduler::block() {
	unsigned long f = arch::cpuIrqSave();
	if (hasPendingSignalCurrent()) { arch::cpuIrqRestore(f); return; }   // signal pending: don't block
	g_tasks[g_cur].state = TASK_BLOCKED;
	arch::cpuIrqRestore(f);
	schedule();
}

void Scheduler::wake(Task* t) {
	// Only a BLOCKED task may be woken. Waking unconditionally could resurrect a TASK_ZOMBIE
	// or TASK_DONE (e.g. procExit waking a not-yet-reaped zombie parent), turning it runnable
	// again — it would re-enter its final for(;;) and burn the CPU forever. READY/RUNNING is a
	// harmless no-op; ZOMBIE/DONE/STOPPED/FREE must stay untouched.
	if (t && t->state == TASK_BLOCKED) {
		t->state = TASK_READY;
		g_needResched = true;   // consider the freshly-ready task at the next safe point
	}
}

// Resume a job-control-STOPPED task (SIGCONT, or SIGKILL so it can run far enough to die).
// Kept distinct from wake() so an ordinary wakeup (I/O ready, a child's SIGCHLD) can NEVER
// un-stop a Ctrl+Z'd process — only an explicit continue/kill does.
void Scheduler::resume(Task* t) {
	if (t && t->state == TASK_STOPPED) {
		t->state = TASK_READY;
		g_needResched = true;
	}
}

// Park the current task on `q` until wakeAll() (or a signal) makes it READY again, then
// unlink it. The queue add + BLOCKED flip are interrupts-off so a wakeAll from an IRQ can't
// slip between them and lose the wakeup; if it fires in the gap before schedule(), it simply
// finds us READY and schedule() keeps/repicks us — never a missed event.
void Scheduler::sleepOn(WaitQueue* q) {
	if (!q) { ioWait(); return; }            // no queue (shouldn't happen): degrade to tick poll
	unsigned long f = arch::cpuIrqSave();
	// Re-check under interrupts-off: with IF=1 syscalls a signal IRQ may have fired between the
	// caller's condition test and here. If one is pending, don't sleep — the caller re-checks
	// and returns -ERESTARTSYS, so a Ctrl+C can't be lost into an indefinite block.
	if (hasPendingSignalCurrent()) { arch::cpuIrqRestore(f); return; }
	q->add(&g_tasks[g_cur]);
	g_tasks[g_cur].state = TASK_BLOCKED;
	arch::cpuIrqRestore(f);
	schedule();
	f = arch::cpuIrqSave();
	q->remove(&g_tasks[g_cur]);              // resumed: we no longer wait on q (woken or signalled)
	arch::cpuIrqRestore(f);
}

// As sleepOn, but loops until `ready(ctx)` holds (or a signal is pending), re-testing the
// condition under interrupts-off with the task already enqueued. This closes the lost-wakeup
// window for an IRQ-driven waker (the keyboard filling the console buffer): a wake that fires
// after the test but before we park finds us on the queue.
void Scheduler::sleepOnUntil(WaitQueue* q, bool (*ready)(void*), void* ctx) {
	if (!q) return;
	for (;;) {
		unsigned long f = arch::cpuIrqSave();
		if (ready(ctx) || hasPendingSignalCurrent()) { arch::cpuIrqRestore(f); return; }
		q->add(&g_tasks[g_cur]);
		g_tasks[g_cur].state = TASK_BLOCKED;
		arch::cpuIrqRestore(f);
		schedule();
		f = arch::cpuIrqSave();
		q->remove(&g_tasks[g_cur]);
		arch::cpuIrqRestore(f);
	}
}

// Make every task parked on `q` runnable (the object became readable/writable, or closed).
// Tasks unlink themselves in sleepOn on return, so we only flip states here.
// True iff `t` points at a real, aligned slot of the static task array — a defensive guard so a
// corrupted/dangling WaitQueue head or waitNext can't send the walk into wild memory and fault.
static bool isTaskSlot(const Task* t) {
	if (t < &g_tasks[0] || t >= &g_tasks[MAXTASKS])
		return false;
	return (((const char*) t - (const char*) &g_tasks[0]) % sizeof(Task)) == 0;
}

void Scheduler::wakeAll(WaitQueue* q) {
	if (!q) return;
	unsigned long f = arch::cpuIrqSave();
	// Bound the walk by the slot count and validate every link: a wakeAll on a corrupt queue
	// then degrades to a no-op instead of dereferencing garbage (the parked task array is fixed,
	// so any pointer outside it is corruption — stop rather than fault).
	Task* t = q->head;
	for (int guard = 0; t && guard <= MAXTASKS; guard++, t = t->waitNext) {
		if (!isTaskSlot(t))
			break;
		if (t->state == TASK_BLOCKED)
			t->state = TASK_READY;
	}
	g_needResched = true;
	arch::cpuIrqRestore(f);
}

void Scheduler::reap(Task* t) {
	if (!t)
		return;
	if (t->kstack) { free(t->kstack); t->kstack = 0; }   // return the kernel stack to the heap
	t->state = TASK_FREE;       // slot becomes reusable by findFreeSlot() (with a fresh stack)
}

void Scheduler::start() {
	int first = pickNext(0);
	g_cur = first;
	g_tasks[first].state = TASK_RUNNING;
	arch::setKernelStack(g_tasks[first].esp0);
	ProcTable::setCurrent(g_tasks[first].proc);
	ProcTable::setCurrentThread(g_tasks[first].thread);
	arch::archLoadThreadTls(g_tasks[first].thread ? g_tasks[first].thread->tlsBase : 0);
	// Switch from the throwaway boot context into the first task; never returns here.
	static uintptr_t throwaway;
	arch::archContextSwitch(&throwaway, g_tasks[first].kesp);
}

void Scheduler::runCurrentBody() {
	// A freshly-bootstrapped task starts executing kernel code here, reached directly via the
	// arch task trampoline (NOT through schedule()'s re-acquire), so it must take the BKL itself.
	// The matching release is whichever way the task leaves the kernel: schedule()/block() (the
	// handoff exit), exit-to-ring-3 (archEnterUser's bklExit), or its final schedule() when body()
	// returns (TASK_DONE).
	g_bkl.enter();
	g_tasks[g_cur].body();
	g_tasks[g_cur].state = TASK_DONE;
	schedule();
	for (;;) {}   // unreachable: a DONE task is never rescheduled
}

}  // namespace kernel
