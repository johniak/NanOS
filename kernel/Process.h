/*
 * Process.h — the process table.
 *
 * A process owns a scheduler task, an address space, and per-process syscall state
 * (the fd table + exit status live inside its own Syscalls instance). The kernel
 * tracks the running process so the syscall dispatch routes to its Syscalls; fork
 * allocates a new process and copies the parent's state.
 */
#ifndef PROCESS_H_
#define PROCESS_H_

#include "Signal.h"

namespace kernel {

struct Task;       // scheduler task (Scheduler.h)
class Syscalls;    // per-process syscall state incl. the fd table (Syscall.h)

struct Process {
	bool used;
	int pid;
	int parent;          // parent's pid (for waitpid)
	Task* task;          // scheduler context
	void* space;         // arch::AddressSpace* (opaque here)
	Syscalls* sys;       // per-process fd table + exit status
	bool exited;         // true once it has called exit() (awaiting reap)
	int exitCode;        // valid once `exited`
	int termSignal;      // 0 = exited normally; else the signal that killed it
	bool kthread;        // kernel thread (idle/clock): no user address space
	char comm[16];       // short name (Linux `comm`)
	char cmdline[128];   // full command line (argv joined by spaces)

	// Signals + job control.
	SignalState sig;     // pending/blocked masks + disposition table
	bool stopped;        // job-control stopped (its task is TASK_STOPPED)
	int  stopSignal;     // the signal that stopped it (valid while `stopped`)
	bool stopReported;   // waitpid(WUNTRACED) has already reported this stop
	bool continued;      // SIGCONT delivered since the last wait report
};

// A read-only snapshot of one process, the source for /proc and ps. Decoupled from
// the live table so the (pure) renderers and the SynthFs /proc layer are host-testable.
struct ProcInfo {
	int pid;
	int ppid;
	char state;          // 'R' running/ready, 'S' sleeping (blocked), 'Z' zombie
	bool kthread;
	char comm[16];
	char cmdline[128];
};

class ProcTable {
public:
	static void init();
	static Process* alloc(int parent);   // a free slot with a fresh pid, or 0
	static Process* current();           // the running process (0 before set)
	static void setCurrent(Process* p);
	static Process* byPid(int pid);
	static Process* byTask(Task* t);     // the process whose scheduler task is t

	// waitpid lookup (pure bookkeeping; the scheduler/arch teardown is the caller's).
	// Scan the children of `parentPid` (wantPid > 0 narrows to that one child):
	//   - an exited child -> set *childOut to it and return its pid (the caller tears
	//     it down and frees the slot via freeSlot);
	//   - matching child(ren) still running -> return 0 (caller should block);
	//   - no matching child -> return -10 (-ECHILD).
	static int reapChild(int parentPid, int wantPid, Process** childOut);
	static void freeSlot(Process* p);    // release a process slot after teardown

	// /proc + ps support.
	static int snapshot(ProcInfo* out, int max);     // fill `out`, return live count
	static bool infoByPid(int pid, ProcInfo* out);   // one process, false if absent
	static void setCommand(Process* p, const char* const* argv, int argc);  // comm + cmdline
};

}

#endif /* PROCESS_H_ */
