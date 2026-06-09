#include "Exec.h"
#include "NxeLoader.h"
#include "DynLoader.h"
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "SignalDispatch.h"
#include "Process.h"
#include "Scheduler.h"
#include "String.h"
#include <arch/usermode.h>
#include <arch/mmu.h>
#include <arch/sched.h>

namespace kernel {

// The foreground process: the pid the shell is currently blocked in waitpid() on. The
// console interrupt (Ctrl+C) targets it, mirroring a Unix tty's foreground process
// group. 0 = no foreground (the shell is at its prompt).
static int g_foregroundPid = 0;

// Reset a process's brk/sbrk heap to empty (no pages mapped yet) at the fixed high-VA
// base. Called whenever a fresh address space is installed (program launch / execve).
static void initBrk(Process* p) {
	p->brkBase = arch::mmuUserHeapBase();
	p->brkCur = p->brkBase;
	p->brkMax = arch::mmuUserHeapMax();
}

// The staging window: the reserved 1 MiB at 0x400000 (see mmu_x86.cpp markRangeUsed).
// The image is read here, bss is zeroed in place, then archLoadUser copies it into the
// process's private frames. loadImage bounds every access to this capacity, so an image
// whose bss/tables would overrun the window is rejected cleanly instead of corrupting RAM.
static const unsigned STAGE_BASE = 0x400000;
static const unsigned STAGE_CAP  = 0x100000;

// Load a .nxe image (already staged at the load base in the kernel identity window),
// applying relocations + zeroing bss. EXEs load at their preferred base, so the delta is
// 0 and relocation is a no-op. Returns the entry point, or <0 on error. Caller must be on
// a directory where the staging window 0x400000 is identity-mapped.
static int loadStaged(unsigned* entryOut) {
	char* image = (char*) STAGE_BASE;
	return NxeLoader::loadImage(image, STAGE_CAP, 0, 0, entryOut);   // delta 0, no imports
}

// PID 1 launch (init task body, kernel directory active): stage the image, build a
// fresh address space for it, enter ring 3. Does not return on success.
int execProgram(Vfs* vfs, const char* path) {
	String p = String((char*) path);
	FileStat st;
	if (vfs->stat(p, st) < 0)
		return -1;
	char* image = (char*) STAGE_BASE;
	if (vfs->read(p, st.size, 0, image) < 0)
		return -1;
	NxHeader* h = (NxHeader*) image;
	unsigned entry = 0;
	// A program that imports symbols / needs shared libraries goes through the dynamic
	// linker (loads its .ndl deps into the new space first); otherwise the simple path.
	arch::AddressSpace* space = arch::mmuCreateAddressSpace();
	int rc = (h->neededCount || h->importCount)
			? dynLoadProgram(vfs, image, STAGE_CAP, space, &entry)
			: loadStaged(&entry);
	if (rc < 0) {
		arch::mmuFreeAddressSpace(space);
		return rc;
	}
	const char* argv[] = { path, 0 };
	const char* envp[] = {                               // PID 1's baseline environment
		"TERM=xterm-256color",
		"TERMINFO=/disks/main/nanos/share/terminfo",
		0,
	};
	unsigned esp = arch::archLoadUser(space, h->loadBase, h->bssEnd, argv, 2, envp, 2);
	ProcTable::current()->space = space;
	initBrk(ProcTable::current());
	ProcTable::setCommand(ProcTable::current(), argv, 1);
	kernelSyscalls()->resetForRun();
	arch::archEnterUser(entry, esp, space);   // never returns
	return 0;                                 // unreachable
}

// execve(2): replace the current process's image. We run inside a syscall on the
// process's user CR3; stage + load under the kernel directory (where 0x400000 is
// identity-mapped), then rewrite the trap frame so the iret enters the new image.
int execve(Vfs* vfs, const char* path, const char* const* argv, int argc,
		const char* const* envp, int envc, arch::TrapFrame* tf) {
	Process* p = ProcTable::current();
	unsigned userDir = arch::mmuCurrentDirPhys();
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());

	String pp = String((char*) path);
	FileStat st;
	if (vfs->stat(pp, st) < 0) {
		arch::mmuLoadDirPhys(userDir);
		return -2;   // -ENOENT
	}
	char* image = (char*) STAGE_BASE;
	if (vfs->read(pp, st.size, 0, image) < 0) {
		arch::mmuLoadDirPhys(userDir);
		return -1;
	}
	NxHeader* h = (NxHeader*) image;
	unsigned entry = 0;

	// Load into a fresh space (dynamic linker if the image imports/needs libraries),
	// then drop the caller's old image.
	arch::AddressSpace* newSpace = arch::mmuCreateAddressSpace();
	int rc = (h->neededCount || h->importCount)
			? dynLoadProgram(vfs, image, STAGE_CAP, newSpace, &entry)
			: loadStaged(&entry);
	if (rc < 0) {
		arch::mmuFreeAddressSpace(newSpace);
		arch::mmuLoadDirPhys(userDir);
		return rc;
	}
	unsigned esp = arch::archLoadUser(newSpace, h->loadBase, h->bssEnd, argv, argc, envp, envc);
	if (p->space)
		arch::mmuFreeAddressSpace((arch::AddressSpace*) p->space);
	p->space = newSpace;
	initBrk(p);                              // fresh image -> empty heap
	ProcTable::setCommand(p, argv, argc);
	p->execed = true;                        // POSIX: a child cannot be setpgid'd after exec
	sigExecReset(p->sig);                    // caught handlers -> default across exec
	kernelSyscalls()->resetForRun();

	arch::archFrameToUser(tf, entry, esp);   // iret will enter the new program ...
	arch::mmuSwitch(newSpace);               // ... under the new address space.
	return 0;                                // value irrelevant (frame rewritten)
}

// fork(2): eager copy of the current process. Copy the address space under the kernel
// directory (where all RAM is identity-mapped, so the user frames are reachable), dup
// the fd table, and fabricate the child's kernel stack from the parent's trap frame.
int forkProcess(arch::TrapFrame* tf) {
	Process* parent = ProcTable::current();
	Process* child = ProcTable::alloc(parent->pid);
	if (!child)
		return -11;   // -EAGAIN: no free process slot

	unsigned parentDir = arch::mmuCurrentDirPhys();
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());
	arch::AddressSpace* space = arch::mmuCopyAddressSpace((arch::AddressSpace*) parent->space);
	arch::mmuLoadDirPhys(parentDir);
	if (!space) {
		child->used = false;
		return -11;
	}
	child->space = space;
	child->brkBase = parent->brkBase;          // inherit the heap (mmuCopyAddressSpace
	child->brkCur = parent->brkCur;            // already duplicated the mapped pages)
	child->brkMax = parent->brkMax;
	child->sys = new Syscalls(*parent->sys);   // dup the parent's fd table
	child->kthread = false;
	for (int i = 0; i < (int) sizeof child->comm; i++)
		child->comm[i] = parent->comm[i];      // inherit name until the child exec's
	for (int i = 0; i < (int) sizeof child->cmdline; i++)
		child->cmdline[i] = parent->cmdline[i];
	child->pgid = parent->pgid;                // inherit the process group + session
	child->sid = parent->sid;
	sigForkInherit(child->sig, parent->sig);   // inherit dispositions + mask (no pending)

	Task* t = Scheduler::createBlank(child->pid);
	child->task = t;
	arch::archForkChild(t, tf, arch::mmuSpaceDirPhys(space));
	t->state = TASK_READY;                      // scheduler picks it up; resumes with eax=0
	return child->pid;                          // parent sees the child's pid
}

// When a process exits it may orphan one of its children's process groups (POSIX): a group
// that loses its last live, in-session, out-of-group parent. If such a group has stopped
// members, they must receive SIGHUP then SIGCONT so they are not left blocked forever.
// Call AFTER marking the dying process exited, so the orphan test sees it as gone.
static void orphanCheckOnExit(Process* dying) {
	ProcInfo arr[ProcTable::MAX];
	int n = ProcTable::snapshot(arr, ProcTable::MAX);
	int done[ProcTable::MAX], nd = 0;
	for (int i = 0; i < n; i++) {
		if (arr[i].ppid != dying->pid || arr[i].pgid == dying->pgid)
			continue;
		int g = arr[i].pgid;
		bool seen = false;
		for (int k = 0; k < nd; k++) if (done[k] == g) seen = true;
		if (seen)
			continue;
		done[nd++] = g;
		if (ProcTable::isOrphanedGroup(g) && ProcTable::groupHasStopped(g)) {
			signalSendGroup(g, SIGHUP);
			signalSendGroup(g, SIGCONT);
		}
	}
}

// SYS_exit tail: free the address space we are standing on (after switching to the
// kernel directory so we never free the live CR3), zombify the task, schedule away.
void procExit() {
	Process* p = ProcTable::current();
	p->exitCode = p->sys->code();
	p->exited = true;
	orphanCheckOnExit(p);    // re-parent fallout: SIGHUP+SIGCONT any newly-orphaned stopped group
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());
	if (p->space) {
		arch::mmuFreeAddressSpace((arch::AddressSpace*) p->space);
		p->space = 0;
	}
	// Wake the parent if it is blocked in waitpid; it will reap this zombie.
	Process* parent = ProcTable::byPid(p->parent);
	if (parent)
		Scheduler::wake(parent->task);
	Scheduler::current()->state = TASK_ZOMBIE;
	Scheduler::schedule();   // never returns to this (now zombie) task
	for (;;) {}              // unreachable
}

// waitpid(2): wait on a child of the current process. With WUNTRACED, also report a
// child that has just stopped (job control). With WNOHANG, return 0 instead of blocking
// when nothing is reportable. Writes the W*-encoded status to *statusOut and returns the
// child pid (freeing the slot only for an exited child). -ECHILD if there is no child.
enum { WAIT_WNOHANG = 1, WAIT_WUNTRACED = 2 };
int waitProcess(int wantPid, int* statusOut, int options) {
	Process* parent = ProcTable::current();
	int prevForeground = g_foregroundPid;
	if (wantPid > 0)
		g_foregroundPid = wantPid;   // Ctrl+C / Ctrl+Z target the child we are waiting on
	for (;;) {
		Process* child = 0;
		int r = ProcTable::reapChild(parent->pid, wantPid, &child);
		if (r == -10) {
			g_foregroundPid = prevForeground;
			return -10;            // -ECHILD: no such child
		}
		if (r > 0) {
			int code = child->exitCode;
			int sigd = child->termSignal;
			Scheduler::reap(child->task);   // free the child's task slot (kstack reuse)
			delete child->sys;              // dup'd fd table from fork
			ProcTable::freeSlot(child);     // release the process slot
			if (statusOut)                  // signal death vs normal exit (W* encoding)
				*statusOut = sigd ? waitStatusSignalled(sigd) : waitStatusExited(code);
			g_foregroundPid = prevForeground;
			return r;
		}
		// A child that just stopped (job control): report it without reaping the slot.
		if (options & WAIT_WUNTRACED) {
			Process* st = 0;
			int sp = ProcTable::reapStopped(parent->pid, wantPid, &st);
			if (sp > 0) {
				if (statusOut)
					*statusOut = waitStatusStopped(st->stopSignal);
				g_foregroundPid = prevForeground;
				return sp;
			}
		}
		if (options & WAIT_WNOHANG) {       // nothing reportable; don't block
			g_foregroundPid = prevForeground;
			return 0;
		}
		Scheduler::block();        // children alive but none reportable yet: wait
		if (hasPendingSignalCurrent()) {   // a signal for US interrupts the wait
			g_foregroundPid = prevForeground;
			return -4;             // -EINTR
		}
	}
}

// ----- Signals -------------------------------------------------------------------
//
// Delivery always runs in the TARGET's own context, at its return-to-user point (a
// syscall return or an IRQ return to ring 3). So the terminate/stop paths just act on
// the current process and schedule away; posting from another context only sets the
// pending bit (and wakes a blocked target so it reaches a return-to-user).

namespace { unsigned sigbit(int s) { return 1u << (s - 1); } }

// Terminate the current process because of a fatal signal. Mirrors procExit but records
// the killing signal (so waitpid reports WIFSIGNALED). Does NOT return.
static void procKill(int sig) {
	Process* p = ProcTable::current();
	p->termSignal = sig;
	p->exitCode = sig;
	p->exited = true;
	orphanCheckOnExit(p);    // same orphan-group handling as a normal exit
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());
	if (p->space) {
		arch::mmuFreeAddressSpace((arch::AddressSpace*) p->space);
		p->space = 0;
	}
	Process* parent = ProcTable::byPid(p->parent);
	if (parent)
		Scheduler::wake(parent->task);
	Scheduler::current()->state = TASK_ZOMBIE;
	Scheduler::schedule();
	for (;;) {}   // unreachable
}

// Stop the current process (job-control SIGTSTP/SIGSTOP). Notifies the parent (SIGCHLD +
// wake, so waitpid(WUNTRACED) reports the stop) and deschedules. RETURNS when a later
// SIGCONT marks the task runnable again — the trap frame is untouched, so the eventual
// iret resumes the user exactly where it stopped.
static void procStop(int sig) {
	Process* p = ProcTable::current();
	p->stopped = true;
	p->stopSignal = sig;
	p->stopReported = false;
	Process* parent = ProcTable::byPid(p->parent);
	if (parent) {
		sigPost(parent->sig, SIGCHLD);
		Scheduler::wake(parent->task);
	}
	Scheduler::current()->state = TASK_STOPPED;
	Scheduler::schedule();         // resumes here once continued (or killed)
	p->stopped = false;
}

// kill(2): post `sig` to process `pid`. Wakes a blocked target so it can deliver. Per POSIX
// a non-positive pid targets a process group: pid == 0 is the caller's group, pid < 0 is
// the group |pid|.
int signalSend(int pid, int sig) {
	if (sig < 0 || sig >= NANOS_NSIG)
		return -22;   // -EINVAL
	if (pid == -1) {  // broadcast: every process we may signal, except init (pid 1) and self
		Process* me = ProcTable::current();
		int self = me ? me->pid : 0;
		ProcInfo arr[ProcTable::MAX];
		int n = ProcTable::snapshot(arr, ProcTable::MAX);
		int rc = -3;
		for (int i = 0; i < n; i++) {
			if (arr[i].pid == 1 || arr[i].pid == self || arr[i].kthread)
				continue;
			if (signalSend(arr[i].pid, sig) == 0)
				rc = 0;
		}
		return rc;
	}
	if (pid <= 0) {
		Process* me = ProcTable::current();
		int pgid = (pid == 0) ? (me ? me->pgid : 0) : -pid;
		return signalSendGroup(pgid, sig);
	}
	Process* t = ProcTable::byPid(pid);
	if (!t)
		return -3;    // -ESRCH
	if (sig == 0)
		return 0;     // existence check only
	sigPost(t->sig, sig);
	if (sig == SIGCONT && t->stopped) {        // resume a stopped process (job control)
		t->stopped = false;
		t->continued = true;
		Process* parent = ProcTable::byPid(t->parent);
		if (parent) { sigPost(parent->sig, SIGCHLD); Scheduler::wake(parent->task); }
	}
	// Wake the target so it reaches a return-to-user and delivers: any blocked task, or a
	// stopped task that is being continued or killed.
	if (t->task) {
		if (t->task->state == TASK_BLOCKED)
			Scheduler::wake(t->task);
		else if (t->task->state == TASK_STOPPED && (sig == SIGCONT || sig == SIGKILL))
			Scheduler::wake(t->task);
	}
	return 0;
}

// signal(2): install a disposition for the current process. `handler` is kSigDefault,
// kSigIgnore, or a user function address; `restorer` (the libc sigreturn trampoline) is
// remembered when nonzero. Returns the previous disposition.
int signalAction(int sig, unsigned handler, unsigned restorer) {
	if (sig <= 0 || sig >= NANOS_NSIG || !sigCanCatch(sig))
		return -22;   // -EINVAL
	Process* p = ProcTable::current();
	unsigned prev = p->sig.handlers[sig];
	p->sig.handlers[sig] = handler;
	if (restorer)
		p->sig.restorer = restorer;
	// signal() carries SA_RESTART (glibc/BSD semantics): a handler restarts interrupted
	// syscalls. Default/ignore dispositions clear it.
	if (handler != kSigDefault && handler != kSigIgnore)
		p->sig.restart |= sigbit(sig);
	else
		p->sig.restart &= ~sigbit(sig);
	return (int) prev;
}

// sigprocmask(2): how 0=BLOCK, 1=UNBLOCK, 2=SETMASK. SIGKILL/SIGSTOP stay unblockable.
int signalMask(int how, unsigned set, unsigned* oldset) {
	Process* p = ProcTable::current();
	if (oldset)
		*oldset = p->sig.blocked;
	switch (how) {
	case 0: p->sig.blocked |= set; break;
	case 1: p->sig.blocked &= ~set; break;
	case 2: p->sig.blocked = set; break;
	default: return -22;   // -EINVAL
	}
	p->sig.blocked &= ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
	return 0;
}

bool hasPendingSignalCurrent() {
	Process* p = ProcTable::current();
	return p && sigHasInterrupt(p->sig);   // ignored signals (SIGCHLD) must not cause EINTR
}

// Deliver pending signals at a return-to-user point. The caller has already ensured the
// trap frame returns to ring 3.
void signalDeliver(arch::TrapFrame* tf, unsigned origEax, bool inSyscall) {
	Process* p = ProcTable::current();
	if (!p || p->kthread)
		return;
	// Was this a blocking syscall interrupted by a signal (eligible for restart/EINTR)?
	bool restartable = inSyscall && (arch::archSyscallResult(tf) == -ERESTARTSYS);
	for (;;) {
		int sig = sigNextDeliverable(p->sig);
		if (!sig)
			break;
		sigConsume(p->sig, sig);
		switch (sigResolve(p->sig, sig)) {
		case DISP_IGN:
		case DISP_CONT:
			continue;                  // ignored / resume is handled when SIGCONT is posted
		case DISP_STOP:
			procStop(sig);             // stop here; returns when continued (then restart below)
			continue;
		case DISP_HANDLER: {
			// Run the user handler in ring 3, with `sig` blocked for its duration. Bake the
			// post-handler resume into the frame: restart the interrupted syscall (SA_RESTART)
			// or report -EINTR.
			int action = arch::SIG_FRAME_KEEP;
			if (restartable)
				action = (p->sig.restart & sigbit(sig)) ? arch::SIG_FRAME_RESTART
				                                        : arch::SIG_FRAME_EINTR;
			unsigned oldMask = p->sig.blocked;
			p->sig.blocked |= sigbit(sig);
			arch::archPushSignalFrame(tf, p->sig.handlers[sig], p->sig.restorer,
					sig, oldMask, origEax, action);
			return;                    // one handler per return-to-user; rest after sigreturn
		}
		case DISP_TERM:
		default:
			procKill(sig);             // frees space, zombifies, wakes parent; no return
		}
	}
	// No handler was set up (only stop/continue happened). A syscall interrupted by a stop
	// restarts once continued — Linux never returns EINTR for a bare stop.
	if (restartable)
		arch::archRestartSyscall(tf, origEax);
}

// SYS_sigreturn: restore the pre-handler context from the user-stack frame and the
// signal mask that was in effect before the handler ran. Returns the interrupted code's
// eax (which the dispatch propagates back into the trap frame).
int signalReturn(arch::TrapFrame* tf) {
	uint32_t oldMask = 0;   // archSigreturn wants a uint32_t* (== unsigned long* on i686)
	int rc = arch::archSigreturn(tf, &oldMask);
	ProcTable::current()->sig.blocked = (unsigned) oldMask;
	return rc;
}

// A control key from the cooked-mode tty (Ctrl+C/Ctrl+\/Ctrl+Z) -> deliver `sig` to the
// foreground process (the child the shell is blocked on in waitpid).
void consoleSignal(int sig) {
	if (g_foregroundPid <= 0)
		return;
	Process* fg = ProcTable::byPid(g_foregroundPid);
	if (fg && !fg->kthread)
		signalSend(fg->pid, sig);
}

// Post `sig` to every member of process group `pgid` (kill(-pgid)/terminal signals). Reuses
// signalSend per member so SIGCONT/stop handling and target wakeups stay in one place.
int signalSendGroup(int pgid, int sig) {
	if (pgid <= 0)
		return -3;    // -ESRCH
	int pids[32];
	int n = ProcTable::groupMembers(pgid, pids, 32);
	if (n == 0)
		return -3;
	int rc = -3;
	for (int i = 0; i < n; i++) {
		Process* p = ProcTable::byPid(pids[i]);
		if (!p || p->kthread)
			continue;
		sigPost(p->sig, sig);
		if (sig == SIGCONT && p->stopped) {
			p->stopped = false;
			p->continued = true;
			Process* par = ProcTable::byPid(p->parent);
			if (par) { sigPost(par->sig, SIGCHLD); Scheduler::wake(par->task); }
		}
		if (p->task) {
			if (p->task->state == TASK_BLOCKED)
				Scheduler::wake(p->task);
			else if (p->task->state == TASK_STOPPED && (sig == SIGCONT || sig == SIGKILL))
				Scheduler::wake(p->task);
		}
		rc = 0;
	}
	return rc;
}

// A terminal control key routed to the tty's FOREGROUND PROCESS GROUP (TIOCSPGRP). Falls
// back to the single foreground pid when no group has claimed the terminal yet.
void consoleSignalGroup(int sig, int pgrp) {
	if (pgrp > 0)
		signalSendGroup(pgrp, sig);
	else
		consoleSignal(sig);
}

// ---- Process-group / session syscalls (glue over ProcTable bookkeeping) --------------
int sysSetpgid(int pid, int pgid) { return ProcTable::setpgid(pid, pgid); }
int sysGetpgid(int pid)           { return ProcTable::getpgid(pid); }
int sysSetsid()                   { return ProcTable::setsid(); }
int sysGetsid(int pid)            { return ProcTable::getsid(pid); }

}
