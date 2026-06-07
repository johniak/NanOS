#include "Process.h"

namespace kernel {

static const int MAXPROC = 16;
static Process g_procs[MAXPROC];
static int g_nextPid = 1;
static Process* g_current = 0;

void ProcTable::init() {
	for (int i = 0; i < MAXPROC; i++)
		g_procs[i].used = false;
	g_nextPid = 1;
	g_current = 0;
}

Process* ProcTable::alloc(int parent) {
	for (int i = 0; i < MAXPROC; i++) {
		if (!g_procs[i].used) {
			Process* p = &g_procs[i];
			p->used = true;
			p->pid = g_nextPid++;
			p->parent = parent;
			p->task = 0;
			p->space = 0;
			p->sys = 0;
			p->exitCode = 0;
			return p;
		}
	}
	return 0;
}

Process* ProcTable::current() { return g_current; }
void ProcTable::setCurrent(Process* p) { g_current = p; }

Process* ProcTable::byPid(int pid) {
	for (int i = 0; i < MAXPROC; i++)
		if (g_procs[i].used && g_procs[i].pid == pid)
			return &g_procs[i];
	return 0;
}

Process* ProcTable::byTask(Task* t) {
	for (int i = 0; i < MAXPROC; i++)
		if (g_procs[i].used && g_procs[i].task == t)
			return &g_procs[i];
	return 0;
}

}  // namespace kernel
