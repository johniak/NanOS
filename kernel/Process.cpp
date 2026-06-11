#include "Process.h"
#include "Scheduler.h"   // Task / TaskState for the /proc state char

namespace kernel {

const int ProcTable::MAX;                    // out-of-line definition for ODR-use
static const int MAXPROC = ProcTable::MAX;   // single source of truth (see Process.h)
static Process g_procs[MAXPROC];
static int g_nextPid = 1;
static Process* g_current = 0;
static unsigned g_cpuUser, g_cpuSystem, g_cpuIdle;   // global CPU ticks (jiffies) by class
static unsigned g_forksTotal;                         // processes ever created (since boot)
static int g_lastPid;                                 // most recently allocated pid

void ProcTable::init() {
	for (int i = 0; i < MAXPROC; i++)
		g_procs[i].used = false;
	g_nextPid = 1;
	g_current = 0;
	g_cpuUser = g_cpuSystem = g_cpuIdle = 0;
	g_forksTotal = 0;
	g_lastPid = 0;
}

Process* ProcTable::alloc(int parent) {
	for (int i = 0; i < MAXPROC; i++) {
		if (!g_procs[i].used) {
			Process* p = &g_procs[i];
			p->used = true;
			p->pid = g_nextPid++;
			p->parent = parent;
			p->pgid = p->pid;        // own group + session by default; fork inherits these,
			p->sid = p->pid;         // setpgid/setsid change them
			p->utime = 0;
			p->stime = 0;
			p->starttime = (unsigned) Scheduler::ticks();
			p->mmapNext = 0;         // lazily set to arch::mmuMmapBase() on first mmap
			p->execed = false;
			g_forksTotal++;
			g_lastPid = p->pid;
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

int ProcTable::reparentChildren(int oldParent, int newParent) {
	int n = 0;
	for (int i = 0; i < MAXPROC; i++)
		if (g_procs[i].used && g_procs[i].parent == oldParent) {
			g_procs[i].parent = newParent;
			n++;
		}
	return n;
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
	out->pgid = p->pgid;
	out->sid = p->sid;
	out->state = stateChar(p);
	out->kthread = p->kthread;
	out->utime = p->utime;
	out->stime = p->stime;
	out->starttime = p->starttime;
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


// ---- Sessions + process groups (job control) -----------------------------------------
// Pure process-table bookkeeping. `pid == 0` selects the current process.

int ProcTable::getpgid(int pid) {
	Process* p = pid ? byPid(pid) : g_current;
	return p ? p->pgid : -3;            // -ESRCH
}

int ProcTable::getsid(int pid) {
	Process* p = pid ? byPid(pid) : g_current;
	return p ? p->sid : -3;
}

// setpgid(pid, pgid): put `pid` into group `pgid` (pgid 0 -> a new group named by the pid).
// Allowed only for the caller itself or one of its children, and only within the caller's
// session; the target group must be the process's own pid (new group) or an existing group
// in the same session.
int ProcTable::setpgid(int pid, int pgid) {
	Process* p = pid ? byPid(pid) : g_current;
	if (!p)
		return -3;                      // -ESRCH
	if (pgid < 0)
		return -22;                     // -EINVAL
	if (pgid == 0)
		pgid = p->pid;
	int cur = g_current ? g_current->pid : 0;
	if (p != g_current && p->parent != cur)
		return -3;                      // not us and not our child
	if (p != g_current && p->execed)
		return -13;                     // -EACCES: a child cannot be moved after it execs
	if (g_current && p->sid != g_current->sid)
		return -1;                      // -EPERM: moving across sessions
	if (pgid != p->pid) {               // joining an existing group: it must be in-session
		Process* leader = byPid(pgid);
		if (!leader || leader->sid != p->sid)
			return -1;                  // -EPERM
	}
	p->pgid = pgid;
	return 0;
}

// setsid(): the caller leaves its group and creates a brand-new session + group it leads
// (sid == pgid == pid). Fails if it is already a process-group leader (POSIX), which a
// freshly forked child never is (it inherited the parent's pgid).
int ProcTable::setsid() {
	Process* p = g_current;
	if (!p)
		return -3;
	if (p->pgid == p->pid)
		return -1;                      // -EPERM: already a group leader
	p->sid = p->pid;
	p->pgid = p->pid;
	return p->sid;
}

int ProcTable::groupMembers(int pgid, int* out, int max) {
	int n = 0;
	for (int i = 0; i < MAXPROC && n < max; i++)
		if (g_procs[i].used && g_procs[i].pgid == pgid)
			out[n++] = g_procs[i].pid;
	return n;
}

// ---- CPU accounting ------------------------------------------------------------------

void ProcTable::accountTick(bool fromUser, bool idle) {
	if (idle) {
		g_cpuIdle++;
		return;
	}
	if (fromUser) {
		if (g_current) g_current->utime++;
		g_cpuUser++;
	} else {
		if (g_current) g_current->stime++;
		g_cpuSystem++;
	}
}

void ProcTable::cpuTimes(unsigned* user, unsigned* system, unsigned* idle) {
	if (user) *user = g_cpuUser;
	if (system) *system = g_cpuSystem;
	if (idle) *idle = g_cpuIdle;
}

unsigned ProcTable::forksTotal() { return g_forksTotal; }
int ProcTable::lastPid() { return g_lastPid; }

// A process group is orphaned when no member has a parent that is alive, in a different
// group, but the same session (POSIX). When a process exit orphans a group with stopped
// members, the kernel must send it SIGHUP + SIGCONT. Pure -> host-tested.
bool ProcTable::isOrphanedGroup(int pgid) {
	bool any = false;
	for (int i = 0; i < MAXPROC; i++) {
		Process* m = &g_procs[i];
		if (!m->used || m->pgid != pgid)
			continue;
		any = true;
		Process* par = byPid(m->parent);
		if (par && par->used && !par->exited && par->pgid != pgid && par->sid == m->sid)
			return false;               // a live outside-but-in-session parent keeps it attached
	}
	return any;                         // empty group is not "orphaned" (nothing to signal)
}

bool ProcTable::groupHasStopped(int pgid) {
	for (int i = 0; i < MAXPROC; i++)
		if (g_procs[i].used && g_procs[i].pgid == pgid && g_procs[i].stopped)
			return true;
	return false;
}

}  // namespace kernel
