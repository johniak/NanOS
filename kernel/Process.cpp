#include "Process.h"
#include "Scheduler.h"   // Task / TaskState for the /proc state char

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
			p->exited = false;
			p->exitCode = 0;
			p->termSignal = 0;
			p->kthread = false;
			p->comm[0] = 0;
			p->cmdline[0] = 0;
			sigInit(p->sig);
			p->stopped = false;
			p->stopSignal = 0;
			p->stopReported = false;
			p->continued = false;
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

int ProcTable::reapChild(int parentPid, int wantPid, Process** childOut) {
	bool any = false;
	for (int i = 0; i < MAXPROC; i++) {
		Process* c = &g_procs[i];
		if (!c->used || c->parent != parentPid)
			continue;
		if (wantPid > 0 && c->pid != wantPid)
			continue;
		any = true;
		if (c->exited) {
			if (childOut)
				*childOut = c;
			return c->pid;
		}
	}
	return any ? 0 : -10;   // 0 = child(ren) still running; -ECHILD = no such child
}

int ProcTable::reapStopped(int parentPid, int wantPid, Process** childOut) {
	for (int i = 0; i < MAXPROC; i++) {
		Process* c = &g_procs[i];
		if (!c->used || c->parent != parentPid)
			continue;
		if (wantPid > 0 && c->pid != wantPid)
			continue;
		if (c->stopped && !c->stopReported) {
			c->stopReported = true;
			if (childOut)
				*childOut = c;
			return c->pid;
		}
	}
	return 0;
}

void ProcTable::freeSlot(Process* p) {
	if (p)
		p->used = false;
}

// Linux-style single-letter run state, derived from the exit flag + scheduler task.
static char stateChar(const Process* p) {
	if (p->exited)
		return 'Z';
	if (p->stopped)
		return 'T';                  // job-control stopped
	if (!p->task)
		return 'R';
	switch (p->task->state) {
	case TASK_BLOCKED: return 'S';   // sleeping (e.g. blocked read / waitpid)
	case TASK_STOPPED: return 'T';   // stopped
	case TASK_ZOMBIE:  return 'Z';
	default:           return 'R';   // READY / RUNNING
	}
}

static void copyName(char* dst, int cap, const char* src) {
	int i = 0;
	for (; src[i] && i < cap - 1; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

static void fillInfo(const Process* p, ProcInfo* out) {
	out->pid = p->pid;
	out->ppid = p->parent;
	out->state = stateChar(p);
	out->kthread = p->kthread;
	copyName(out->comm, sizeof out->comm, p->comm);
	copyName(out->cmdline, sizeof out->cmdline, p->cmdline);
}

int ProcTable::snapshot(ProcInfo* out, int max) {
	int n = 0;
	for (int i = 0; i < MAXPROC && n < max; i++)
		if (g_procs[i].used)
			fillInfo(&g_procs[i], &out[n++]);
	return n;
}

bool ProcTable::infoByPid(int pid, ProcInfo* out) {
	Process* p = byPid(pid);
	if (!p)
		return false;
	fillInfo(p, out);
	return true;
}

void ProcTable::setCommand(Process* p, const char* const* argv, int argc) {
	const char* a0 = (argc > 0 && argv && argv[0]) ? argv[0] : "";
	const char* base = a0;                       // comm = basename(argv[0])
	for (const char* s = a0; *s; s++)
		if (*s == '/')
			base = s + 1;
	copyName(p->comm, sizeof p->comm, base);

	int c = 0;                                   // cmdline = argv joined by spaces
	for (int k = 0; k < argc && argv && argv[k]; k++) {
		if (k && c < (int) sizeof p->cmdline - 1)
			p->cmdline[c++] = ' ';
		for (const char* s = argv[k]; *s && c < (int) sizeof p->cmdline - 1; s++)
			p->cmdline[c++] = *s;
	}
	p->cmdline[c] = 0;
}

}  // namespace kernel
