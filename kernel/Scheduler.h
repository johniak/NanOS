/*
 * Scheduler.h — preemptive round-robin scheduler over a task abstraction.
 *
 * Machine-independent: the task table, states, round-robin selection and the
 * yield/block/wake policy live here. The actual context switch, the fabricated
 * first-run stack and the preemption timer are arch (see <arch/sched.h>).
 *
 * From the scheduler's view every task is a "kernel thread" (a kernel stack + a
 * body function). One task (init) happens to enter ring 3 via execProgram —
 * orthogonal to scheduling. Task 0 is the idle task.
 */
#ifndef SCHEDULER_H_
#define SCHEDULER_H_

#include <stdint.h>   // uintptr_t: kesp/esp0 are kernel stack pointers (64-bit on x86_64)

namespace kernel {

struct Process;   // a task's owning process (Process.h); back-pointer avoids an O(n) byTask scan
struct Thread;    // the thread (within a process) this task runs (Process.h); set when the task binds
struct WaitQueue; // event wait list (WaitQueue.h); sleepOn/wakeAll bridge it to task states

enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_STOPPED, TASK_DONE, TASK_ZOMBIE, TASK_FREE };

// One load-average decay step (the Linux algorithm): every 5 s, blend the three
// exponentially-weighted moving averages toward the current `runnable` count. Values are
// fixed-point with FSHIFT=11 (FIXED_1 = 2048). Pure -> host-tested.
void loadDecay(unsigned load[3], int runnable);

// Write the ABI-default FXSAVE image (FCW=0x037F, MXCSR=0x1F80, all else zero) into a task's
// FPU area. Used by allocSlot for fresh tasks and by execve (fresh image -> default FPU state).
void fpuInitImage(unsigned char* fx);

struct Task {
	// FPU/SSE context (x86_64): the FXSAVE image of this task's ring-3 XMM0-15/MXCSR/x87 state,
	// saved/restored by archContextSwitch. HEAP-allocated per task (an inline array at 1032 tasks
	// added ~0.5 MiB of .bss and overflowed the kernel-image budget below VA_USER_BASE); fxRaw is
	// the malloc'd block (freed by reap), fx the 16-byte-aligned view of it (FXSAVE ISA requires
	// 16-byte alignment). Initialized to the ABI-default image (MXCSR=0x1F80 all-masked,
	// FCW=0x037F) by fpuInitImage; fork/clone overwrite it with the parent's LIVE state
	// (archFpuCapture). Unused-but-harmless on i686, whose frozen userland predates SSE codegen.
	unsigned char* fx;      // 512-byte FXSAVE area, 16-aligned (inside fxRaw)
	unsigned char* fxRaw;   // owning heap block (512+16); freed on reap
	uintptr_t kesp;         // saved kernel esp (the whole context lives on the stack)
	uintptr_t esp0;         // top of this task's kernel stack (TSS.esp0 when it runs)
	TaskState state;
	void (*body)();
	void*    arg;           // opaque per-task argument for arg-passing kernel threads (0 for none)
	int id;                 // 0 = idle
	unsigned char* kstack;
	unsigned wakeAt;        // BLOCKED with a timed wakeup: tick at which onTick re-wakes it (0 = none)
	Process* proc;          // owning process (0 for none) — set when the process binds the task
	Thread*  thread;        // the thread this task runs (0 for none) — set by ProcTable::bindTask
	Task*    waitNext;      // intrusive link while parked on a WaitQueue (see WaitQueue.h)
	int      runningCpu;    // SMP: dense index of the CPU running this task, or -1 if not running
	bool     isIdle;        // SMP: a per-CPU idle task (never entered into the general rotation)
};

class Scheduler {
public:
	static void init();                            // create the idle task (id 0)
	static Task* create(void (*body)(), int id);   // bootstrap a task, mark READY
	// Same, but with an opaque argument the body can read via current()->arg (arg is set before
	// the task is published READY, so a claiming CPU always sees it). For knx_thread_spawn.
	static Task* create(void (*body)(), void* arg, int id);
	static Task* createBlank(int id);              // alloc a task slot + kstack only
	                                               // (kesp fabricated by the caller, e.g. fork)
	static Task* createIdle(int cpu);              // SMP: per-CPU idle task (one per AP)
	static void start();                           // switch into the first runnable task
	static void schedule();                        // pick next runnable + context switch
	static void onTick(bool fromUser);             // BSP PIT: ticks++, CPU-account, wake, flag resched
	static void onTickLocal(bool fromUser);        // SMP: an AP's LAPIC tick — local quantum only
	static void apEnter();                         // SMP: an AP enters the scheduler as its idle task
	static unsigned contextSwitches();             // total context switches (for /proc/stat ctxt)
	static unsigned cpuContextSwitches(int cpu);   // per-CPU switch count (RCU quiescence tracking)
	// Real RCU grace period (LinuxKPI synchronize_rcu). Blocks the caller until every OTHER online
	// CPU has passed through a quiescent state — one that cannot occur inside an RCU read-side
	// section, because NanOS's deferred-preemption scheduler never switches a task in kernel mode
	// except at a voluntary schedule() (and RCU readers never call one). A CPU has quiesced once it
	// has context-switched since the call began OR is observed running its idle task. UP is a
	// barrier (the caller is the only CPU; no concurrent reader can exist).
	static void rcuSynchronize();
	// Pure predicate (host-tested): has the grace period elapsed given the per-CPU switch-count
	// snapshot `snap`, the current counts `now`, whether each CPU is idle right now, which CPUs are
	// online, and the caller's own CPU index (skipped — it can't switch while running this).
	static bool rcuGraceDone(const unsigned* snap, const unsigned* now, const bool* idleNow,
			const bool* online, int n, int selfCpu);
	static void loadAvg(unsigned out[3]);          // 1/5/15-min load in hundredths (/proc/loadavg)
	static void preempt();                         // resched if flagged (called on ret-to-ring3)
	static void yield() { schedule(); }
	static void ioWait();                          // BLOCKED until the next tick (I/O retry loop)
	static void sleepUntil(unsigned tick);         // BLOCKED until g_ticks reaches `tick` (or woken)

	// Pure helpers (host-tested). timedWakeReady: has a task's timed deadline arrived (wrap-safe;
	// wakeAt 0 = no timer). shouldResched: keep the running task until its quantum expires, but
	// preempt promptly when a sleeper just woke — so two CPU-bound tasks don't trade every tick.
	static bool timedWakeReady(unsigned now, unsigned wakeAt);
	static bool shouldResched(unsigned sliceTicks, unsigned quantum, bool wokeSleeper);
	static void block();                           // current -> BLOCKED, then schedule
	static void wake(Task* t);                     // BLOCKED -> READY (IRQ-safe: just a flag)
	static void resume(Task* t);                   // STOPPED -> READY (job-control SIGCONT/KILL)
	static void sleepOn(WaitQueue* q);             // park current on q until wakeAll/signal
	// Park on q, re-testing ready(ctx) under interrupts-off (so an IRQ-driven waker can't be
	// lost), until it holds or a signal is pending. For waiters woken from an IRQ (the console).
	static void sleepOnUntil(WaitQueue* q, bool (*ready)(void*), void* ctx);
	static void wakeAll(WaitQueue* q);             // ready every task parked on q (event fired)
	static void reap(Task* t);                     // -> FREE: release the slot for reuse
	static Task* current();
	static Task* idle();                           // the idle task (slot 0)
	static unsigned ticks();
	static void runCurrentBody();                  // called by the arch trampoline

	// Index of the next task to run, given the current states. The idle task
	// (index 0) is chosen ONLY when no non-idle task is runnable. Pure; host-tested.
	static int nextRunnable(const TaskState* st, int n, int cur);

	// SMP claim policy (pure; host-tested). Index of a CLAIMABLE task — TASK_READY, not an idle
	// task, AND runningCpu == -1 (its context is fully saved, not mid-switch-out on another CPU) —
	// scanning round-robin from curIdx, or -1 if none (the caller then keeps the running task or
	// falls back to its per-CPU idle). The runningCpu gate closes the wake-during-switch-out race:
	// a task woken to READY before its old CPU has saved its kesp is not yet claimable.
	static int pickReady(const TaskState* st, const bool* isIdle, const int* runningCpu,
			int n, int curIdx);
};

}

#endif /* SCHEDULER_H_ */
