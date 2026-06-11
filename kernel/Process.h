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

	// mmap bump pointer: the next free VA in the mmap window. 0 until the first mmap, then
	// initialised to arch::mmuMmapBase() and advanced per mapping. (No unmap reclaim yet.)
	unsigned mmapNext;

	// Sessions + process groups (job control). A new process is its own group+session
	// leader; fork inherits both; setpgid/setsid change them. The tty's foreground process
	// group (TIOCSPGRP) is the one that receives terminal-generated signals (Ctrl+C).
	int pgid;            // process group id (group leader has pgid == pid)
	int sid;             // session id (session leader has sid == pid)

	// CPU accounting (in timer ticks; the timer attributes each tick to the running process,
	// split user vs system by the ring it interrupted). Surfaced in /proc/<pid>/stat.
	unsigned utime;      // ticks spent in ring 3 (user)
	unsigned stime;      // ticks spent in ring 0 on this process's behalf (system)
	unsigned starttime;  // tick count when the process was created (Linux field 22)
	bool execed;         // has called execve at least once (POSIX setpgid restriction)

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
	unsigned utime;      // user / system CPU ticks + creation tick (Linux stat fields 14,15,22)
	unsigned stime;
	unsigned starttime;
	char comm[16];
	char cmdline[128];
};

class ProcTable {
public:
	// Ceiling on live processes (kernel threads included) — a pid-space bound, like Linux's
	// pid_max, not a memory limit: the heavy per-process memory (kernel stack, address space)
	// is allocated dynamically and freed on exit, so the real limit is available RAM. Snapshot
	// buffers are heap-allocated to this size (never on the kernel stack — it is only 8 KB).
	static const int MAX = 1024;
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
	// POSIX orphan handling: re-home every live child of `oldParent` onto `newParent` (init,
	// pid 1) when their parent dies, so they stay reapable instead of leaking. Returns the count.
	static int reparentChildren(int oldParent, int newParent);
	// Visit every live process without a snapshot buffer (so a broadcast signal needs no heap
	// allocation): calls fn(pid, kthread, ctx) per process. The callback must not add/remove
	// processes — posting a signal to each is fine.
	static void forEachLive(void (*fn)(int pid, bool kthread, void* ctx), void* ctx);

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

	// CPU accounting. The timer calls accountTick once per tick: `fromUser` = it interrupted
	// ring 3, `idle` = the idle task was running. It bumps the current process's utime/stime
	// and the global user/system/idle tick counters. cpuTimes/forksTotal/lastPid feed
	// /proc/stat and /proc/loadavg.
	static void accountTick(bool fromUser, bool idle);
	static void cpuTimes(unsigned* user, unsigned* system, unsigned* idle);
	static unsigned forksTotal();        // processes created since boot (Linux /proc/stat)
	static int lastPid();                // pid of the most recently created process
	// Job-control orphan handling: a group is orphaned when no member has a live parent in a
	// different group of the same session; an orphaned group with stopped members must get
	// SIGHUP+SIGCONT when its last attaching process exits. Both pure -> host-tested.
	static bool isOrphanedGroup(int pgid);
	static bool groupHasStopped(int pgid);

	// /proc + ps support.
	static int snapshot(ProcInfo* out, int max);     // fill `out`, return live count
	static bool infoByPid(int pid, ProcInfo* out);   // one process, false if absent
	static void setCommand(Process* p, const char* const* argv, int argc);  // comm + cmdline
};

}

#endif /* PROCESS_H_ */
