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
static void idleBody() { for (;;) arch::halt_or_hlt(); }

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
	for (int i = 0; i < g_ntasks; i++)
		st[i] = g_tasks[i].state;
	int next = nextRunnable(st, g_ntasks, g_cur);
	if (next == g_cur)
		return;                                   // nothing else to run
	int prev = g_cur;
	g_cur = next;
	if (g_tasks[prev].state == TASK_RUNNING)
		g_tasks[prev].state = TASK_READY;
	g_tasks[next].state = TASK_RUNNING;
	arch::archContextSwitch(&g_tasks[prev].kesp, g_tasks[next].kesp);
}

void Scheduler::onTick() {
	g_ticks++;
	schedule();
}

void Scheduler::block() {
	g_tasks[g_cur].state = TASK_BLOCKED;
	schedule();
}

void Scheduler::wake(Task* t) {
	if (t)
		t->state = TASK_READY;
}

void Scheduler::start() {
	TaskState st[MAXTASKS];
	for (int i = 0; i < g_ntasks; i++)
		st[i] = g_tasks[i].state;
	int first = nextRunnable(st, g_ntasks, 0);
	g_cur = first;
	g_tasks[first].state = TASK_RUNNING;
	// Switch from the throwaway boot context into the first task; never returns here.
	static unsigned throwaway;
	arch::archContextSwitch(&throwaway, g_tasks[first].kesp);
}

void Scheduler::runCurrentBody() {
	g_tasks[g_cur].body();
	g_tasks[g_cur].state = TASK_DONE;
	schedule();
	for (;;) {}   // unreachable: a DONE task is never rescheduled
}

}  // namespace kernel
