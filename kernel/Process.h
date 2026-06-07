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
	int exitCode;        // valid once the task is a zombie
};

class ProcTable {
public:
	static void init();
	static Process* alloc(int parent);   // a free slot with a fresh pid, or 0
	static Process* current();           // the running process (0 before set)
	static void setCurrent(Process* p);
	static Process* byPid(int pid);
	static Process* byTask(Task* t);     // the process whose scheduler task is t
};

}

#endif /* PROCESS_H_ */
