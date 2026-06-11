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

namespace kernel {

struct Process;   // a task's owning process (Process.h); back-pointer avoids an O(n) byTask scan
struct WaitQueue; // event wait list (WaitQueue.h); sleepOn/wakeAll bridge it to task states

enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_STOPPED, TASK_DONE, TASK_ZOMBIE, TASK_FREE };

// One load-average decay step (the Linux algorithm): every 5 s, blend the three
// exponentially-weighted moving averages toward the current `runnable` count. Values are
// fixed-point with FSHIFT=11 (FIXED_1 = 2048). Pure -> host-tested.
void loadDecay(unsigned load[3], int runnable);

struct Task {
	unsigned kesp;          // saved kernel esp (the whole context lives on the stack)
	unsigned esp0;          // top of this task's kernel stack (TSS.esp0 when it runs)
	TaskState state;
	void (*body)();
	int id;                 // 0 = idle
	unsigned char* kstack;
	unsigned wakeAt;        // BLOCKED with a timed wakeup: tick at which onTick re-wakes it (0 = none)
	Process* proc;          // owning process (0 for none) — set when the process binds the task
	Task*    waitNext;      // intrusive link while parked on a WaitQueue (see WaitQueue.h)
};

class Scheduler {
public:
	static void init();                            // create the idle task (id 0)
	static Task* create(void (*body)(), int id);   // bootstrap a task, mark READY
	static Task* createBlank(int id);              // alloc a task slot + kstack only
	                                               // (kesp fabricated by the caller, e.g. fork)
	static void start();                           // switch into the first runnable task
	static void schedule();                        // pick next runnable + context switch
	static void onTick(bool fromUser);             // timer: ticks++, CPU-account, wake, flag resched
	static unsigned contextSwitches();             // total context switches (for /proc/stat ctxt)
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
	static void wakeAll(WaitQueue* q);             // ready every task parked on q (event fired)
	static void reap(Task* t);                     // -> FREE: release the slot for reuse
	static Task* current();
	static Task* idle();                           // the idle task (slot 0)
	static unsigned ticks();
	static void runCurrentBody();                  // called by the arch trampoline

	// Index of the next task to run, given the current states. The idle task
	// (index 0) is chosen ONLY when no non-idle task is runnable. Pure; host-tested.
	static int nextRunnable(const TaskState* st, int n, int cur);
};

}

#endif /* SCHEDULER_H_ */
