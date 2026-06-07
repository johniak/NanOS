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

enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_DONE };

struct Task {
	unsigned kesp;          // saved kernel esp (the whole context lives on the stack)
	TaskState state;
	void (*body)();
	int id;                 // 0 = idle
	unsigned char* kstack;
};

class Scheduler {
public:
	static void init();                            // create the idle task (id 0)
	static Task* create(void (*body)(), int id);   // bootstrap a task, mark READY
	static void start();                           // switch into the first runnable task
	static void schedule();                        // pick next runnable + context switch
	static void onTick();                          // timer: ticks++ then schedule
	static void yield() { schedule(); }
	static void block();                           // current -> BLOCKED, then schedule
	static void wake(Task* t);                     // -> READY (IRQ-safe: just a flag)
	static Task* current();
	static unsigned ticks();
	static void runCurrentBody();                  // called by the arch trampoline

	// Index of the next task to run, given the current states. The idle task
	// (index 0) is chosen ONLY when no non-idle task is runnable. Pure; host-tested.
	static int nextRunnable(const TaskState* st, int n, int cur);
};

}

#endif /* SCHEDULER_H_ */
