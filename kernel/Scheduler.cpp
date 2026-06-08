#include "Scheduler.h"
#include "Process.h"
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
	int i = findFreeSlot();
	if (i < 0)
		return 0;
	Task* t = &g_tasks[i];
	t->id = id;
	t->body = 0;
	t->state = TASK_READY;
	t->kstack = g_kstacks[i];
	t->esp0 = (unsigned) (unsigned long) (t->kstack + KSTACK_SIZE);   // TSS.esp0 for this task
	return t;
}

Task* Scheduler::create(void (*body)(), int id) {
	Task* t = allocSlot(id);
	if (!t)
		return 0;
	t->body = body ? body : idleBody;
	t->kesp = arch::archTaskBootstrap(t->kstack + KSTACK_SIZE, arch::archKernelCr3());
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
	arch::setKernelStack(g_tasks[next].esp0);   // ring3 traps land on next's kstack
	ProcTable::setCurrent(ProcTable::byTask(&g_tasks[next]));  // route syscalls to it
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

void Scheduler::reap(Task* t) {
	if (t)
		t->state = TASK_FREE;   // slot becomes reusable by findFreeSlot()
}

void Scheduler::start() {
	TaskState st[MAXTASKS];
	for (int i = 0; i < g_ntasks; i++)
		st[i] = g_tasks[i].state;
	int first = nextRunnable(st, g_ntasks, 0);
	g_cur = first;
	g_tasks[first].state = TASK_RUNNING;
	arch::setKernelStack(g_tasks[first].esp0);
	ProcTable::setCurrent(ProcTable::byTask(&g_tasks[first]));
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
