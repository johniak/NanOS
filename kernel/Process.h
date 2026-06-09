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

	// brk/sbrk heap (the growable anonymous region at a high VA; see arch mmuSetUserBrk).
	unsigned brkBase;    // fixed start of the heap window (== current break when empty)
	unsigned brkCur;     // current program break
	unsigned brkMax;     // hard ceiling (brkBase + cap)

	// Sessions + process groups (job control). A new process is its own group+session
	// leader; fork inherits both; setpgid/setsid change them. The tty's foreground process
	// group (TIOCSPGRP) is the one that receives terminal-generated signals (Ctrl+C).
	int pgid;            // process group id (group leader has pgid == pid)
	int sid;             // session id (session leader has sid == pid)

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
	int pgid;            // process group + session ids (for Linux-format /proc/<pid>/stat)
	int sid;
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

	// Job control: find a child (of parentPid; wantPid > 0 narrows) that is stopped and
	// not yet reported to waitpid(WUNTRACED). Marks it reported and returns its pid via
	// *childOut, or 0 if there is none. Distinct from reapChild (which reaps the dead).
	static int reapStopped(int parentPid, int wantPid, Process** childOut);
	static void freeSlot(Process* p);    // release a process slot after teardown

	// Sessions + process groups. All operate on the current process unless `pid` names
	// another; `pid == 0` means the current process. Pure process-table bookkeeping
	// (host-tested); the syscall glue + signal routing live in Exec.cpp.
	//   setpgid(pid,pgid): join/leave a group. pgid==0 -> pgid=pid (new group). The target
	//     must be the caller or a child in the caller's session; -errno on violation.
	//   setsid(): the caller becomes leader of a brand-new session + group (sid=pgid=pid),
	//     unless it is already a group leader (-EPERM). Returns the new sid.
	static int setpgid(int pid, int pgid);
	static int getpgid(int pid);         // -ESRCH if no such process
	static int setsid();
	static int getsid(int pid);
	// Fill `out` (capacity `max`) with the pids of every live process in group `pgid`;
	// returns the count. Lets the signal layer fan a group signal out, host-tested.
	static int groupMembers(int pgid, int* out, int max);

	// /proc + ps support.
	static int snapshot(ProcInfo* out, int max);     // fill `out`, return live count
	static bool infoByPid(int pid, ProcInfo* out);   // one process, false if absent
	static void setCommand(Process* p, const char* const* argv, int argc);  // comm + cmdline
};

}

#endif /* PROCESS_H_ */
