#include "Exec.h"
#include "NxeLoader.h"
#include "DynLoader.h"
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "SignalDispatch.h"
#include "SyscallNr.h"   // struct k_sigaction: the rt_sigaction kernel-ABI layout
#include "Process.h"
#include "Scheduler.h"
#include "CloneFlags.h"
#include "ThreadArea.h"   // UserDesc: CLONE_SETTLS reads the child's TLS base from it
#include "String.h"
#include "memory_manager.h"   // malloc/free: process-table snapshots go on the heap, not the
                              // 8 KB kernel stack (the table now holds up to ProcTable::MAX)
#include <arch/usermode.h>
#include <arch/mmu.h>
#include <arch/sched.h>

namespace kernel {

// The foreground process: the pid the shell is currently blocked in waitpid() on. The
// console interrupt (Ctrl+C) targets it, mirroring a Unix tty's foreground process
// group. 0 = no foreground (the shell is at its prompt).
static int g_foregroundPid = 0;

// The console's FOREGROUND PROCESS GROUP — the singleton job-control state for the physical
// terminal (the kernel analogue of Pty::m_fgPgrp). Set by tcsetpgrp on a console fd
// (SYS_ioctl TIOCSPGRP) and read by tcgetpgrp / the SIGTTIN gate. 0 = no group has claimed
// the terminal (a shell without job control, e.g. nsh) -> consoleSignal falls back to the
// single waited-on pid above.
static int g_consolePgrp = 0;
void consoleSetPgrp(int pgrp) { g_consolePgrp = pgrp; }
int  consoleGetPgrp()         { return g_consolePgrp; }

// Reset a process's brk/sbrk heap to empty (no pages mapped yet) at the fixed high-VA
// base. Called whenever a fresh address space is installed (program launch / execve).
static void initBrk(Process* p) {
	p->brkBase = arch::mmuUserHeapBase();
	p->brkCur = p->brkBase;
	p->brkMax = arch::mmuUserHeapMax();
}

// The staging window: the reserved 4 MiB at 0x800000 (see mmu_x86.cpp markRangeUsed) — the
// same size as the per-process user window, so any image that fits a process (plus its load-
// time tables) fits here. The image is read here, bss is zeroed in place, then archLoadUser
// copies it into the process's private frames. loadImage bounds every access to this capacity,
// and callers reject an oversize file BEFORE the read, so a large .nxe can never corrupt RAM.
// Base raised 0x400000 -> 0x800000 to give the kernel image headroom (must match user/nx.ld).
static const unsigned STAGE_BASE = 0x800000;
#if defined(__x86_64__)
// x86_64: 32 MiB. A 64-bit .nxe is ~2x its i686 size, so big apps (NetSurf ~16 MiB image+bss,
// file ~8.3 MiB) overflowed 8 MiB. The mmu reserves a matching 32 MiB staging band at VA_USER_BASE
// (arch/x86_64/mm/mmu_x86_64.cpp) and the user VA window was widened to 64 MiB (arch/mmu.h).
static const unsigned STAGE_CAP  = 0x2000000;  // 32 MiB
#else
static const unsigned STAGE_CAP  = 0x800000;   // 8 MiB: matches the i686 per-process user window
#endif

// Load a .nxe image (already staged at the load base in the kernel identity window),
// applying relocations + zeroing bss. EXEs load at their preferred base, so the delta is
// 0 and relocation is a no-op. Returns the entry point, or <0 on error. Caller must be on
// a directory where the staging window 0x800000 is identity-mapped.
static int loadStaged(nxaddr_t* entryOut) {
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
	if (st.size > STAGE_CAP)             // too big to stage -> reject before overrunning the window
		return -1;
	char* image = (char*) STAGE_BASE;
	if (vfs->read(p, st.size, 0, image) < 0)
		return -1;
	NxHeader* h = (NxHeader*) image;
	nxaddr_t entry = 0;
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
		"VIMRUNTIME=/disks/main/apps/vim/runtime",       // vim's runtime (minimal defaults.vim)
		// $VIMINIT is vim's highest-priority init: when set, vim runs it AS the vimrc and does
		// NOT fall back to sourcing $VIMRUNTIME/defaults.vim — which removes the "E1187: Failed
		// to source defaults.vim" prompt while still giving sane editor defaults.
		"VIMINIT=set nocompatible backspace=indent,eol,start hlsearch incsearch ruler showcmd wildmenu",
		0,
	};
	int argc = 0; while (argv[argc]) argc++;             // count, don't hard-code (a stale literal
	int envc = 0; while (envp[envc]) envc++;             // silently truncated newly-added entries)
	unsigned esp = arch::archLoadUser(space, h->loadBase, h->bssEnd, argv, argc, envp, envc);
	ProcTable::current()->space = space;
	initBrk(ProcTable::current());
	ProcTable::current()->mmapNext = 0;          // fresh image -> empty mmap window
	ProcTable::current()->mmapFreeCount = 0;     // and no stale reclaim ranges
	ProcTable::setCommand(ProcTable::current(), argv, 1);
	// init (this process) is the console's controlling session leader: seed the console's
	// foreground process group with its pgrp, exactly as a tty's pgrp is set when a session
	// leader acquires the controlling terminal. Without this a job-control shell (bash) that
	// syncs to the foreground group before its own tcsetpgrp would SIGTTIN-stop itself at boot.
	g_consolePgrp = ProcTable::current()->pgid;
	kernelSyscalls()->resetForRun();
	arch::archEnterUser(entry, esp, space);   // never returns
	return 0;                                 // unreachable
}

// execve(2): replace the current process's image. We run inside a syscall on the
// process's user CR3; stage + load under the kernel directory (where 0x800000 is
// identity-mapped), then rewrite the trap frame so the iret enters the new image.
int execve(Vfs* vfs, const char* path, const char* const* argv, int argc,
		const char* const* envp, int envc, arch::TrapFrame* tf) {
	Process* p = ProcTable::current();
	unsigned userDir = arch::mmuCurrentDirPhys();
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());

	// Resolve the program path against the process cwd (so `./prog` and relative paths work),
	// the same way every other path syscall does.
	String pp = p->sys->resolvePath(String((char*) path));
	FileStat st;
	if (vfs->stat(pp, st) < 0) {
		arch::mmuLoadDirPhys(userDir);
		return -2;   // -ENOENT
	}
	if (st.size > STAGE_CAP) {            // too big to stage -> reject before overrunning the window
		arch::mmuLoadDirPhys(userDir);
		return -1;
	}
	int xc = vfs->checkExec(pp);          // execute permission on the file (+ search on ancestors)
	if (xc < 0) {
		arch::mmuLoadDirPhys(userDir);
		return xc;
	}
	char* image = (char*) STAGE_BASE;
	if (vfs->read(pp, st.size, 0, image) < 0) {
		arch::mmuLoadDirPhys(userDir);
		return -1;
	}
	NxHeader* h = (NxHeader*) image;
	nxaddr_t entry = 0;

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

	// ---- POINT OF NO RETURN ----------------------------------------------------------------
	// The new image loaded cleanly, so we now commit to replacing the process. POSIX: execve in a
	// multithreaded process destroys every OTHER thread in the group; the calling thread survives
	// as the new image's sole (leader) thread. Do this BEFORE dropping the old address space (the
	// siblings still live in p->space) — and only after a successful load, so a failed execve
	// returns to a fully intact thread group.
	Thread* caller = ProcTable::currentThread();
	Task*   self   = Scheduler::current();
	// Reap each sibling's scheduler task (kstack/slot) AND freeThread it. Unlike exit_group (which
	// leaves the thread-list drain to freeSlot at reap time), the process keeps living here, so its
	// thread list + threadCount must end correct. freeThread unlinks the node it frees, so we can't
	// hold a `->next` across it: rescan from the head for the next non-caller victim each pass.
	for (;;) {
		Thread* victim = 0;
		for (Thread* th = p->threads; th; th = th->next)
			if (th != caller) { victim = th; break; }
		if (!victim)
			break;
		if (victim->task) {
			if (p->task == victim->task) p->task = self;   // never leave p->task on a reaped slot
			futexRemoveTask(p->space, victim->task);       // evict its futex waiter before its
			Scheduler::reap(victim->task);                 // kstack (which hosts the node) is freed
		}
		ProcTable::freeThread(victim);   // unlink + threadCount-- (the process survives)
	}
	// Re-designate the calling thread as the group leader so the post-exec invariants hold (the
	// common case — exec from a single-threaded process or a freshly-forked child — already had
	// the caller AS the leader, so this is a no-op there). After the loop the caller is the only
	// remaining thread, hence the head of p->threads (== leaderThread()); give it the leader tid
	// (== pid/tgid, what gettid() must report post-exec) and make it the process's primary task.
	caller->tid = p->pid;
	p->task = self;
	p->threadCount = 1;

	if (p->space)
		arch::mmuFreeAddressSpace((arch::AddressSpace*) p->space);
	p->space = newSpace;
	initBrk(p);                              // fresh image -> empty heap
	p->mmapNext = 0;                         // fresh image -> empty mmap window (don't inherit the
	p->mmapFreeCount = 0;                    // old image's bump pointer / stale reclaim ranges)
	ProcTable::setCommand(p, argv, argc);
	p->execed = true;                        // POSIX: a child cannot be setpgid'd after exec
	sigExecReset(caller->sig, p->psig);      // caught handlers -> default across exec (calling thread)
	p->sys->closeCloexec();                  // FD_CLOEXEC descriptors do not survive exec
	credOnExec(p->cred, st.uid, st.gid, st.mode);   // honor setuid/setgid bits (su/sudo/passwd)
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
		ProcTable::freeSlot(child);   // releases the process slot AND its leader thread slot
		return -11;
	}
	// The copy duplicates the WHOLE address space, so if the parent was multithreaded the child's
	// space still contains the sibling threads' user stacks + TLS blocks. That is harmless dead
	// memory: the child has only one (leader) thread, nothing references those pages, and a child
	// that forks-then-execs (the bash idiom) drops the whole space at exec anyway.
	child->space = space;
	child->brkBase = parent->brkBase;          // inherit the heap (mmuCopyAddressSpace
	child->brkCur = parent->brkCur;            // already duplicated the mapped pages)
	child->brkMax = parent->brkMax;
	// The mmap window was duplicated frame-by-frame, so the child resumes the bump pointer
	// where the parent left off (everything below is already mapped). The reclaim free-list
	// starts EMPTY (zeroed by alloc) — the parent's freed holes are just unallocated VA the
	// child will bump past, never stale entries pointing into the child's own space.
	child->mmapNext = parent->mmapNext;
	child->sys = new Syscalls(*parent->sys);   // dup the parent's fd table
	child->cred = parent->cred;                // inherit credentials
	child->sys->setCred(&child->cred);         // point at the CHILD's canonical Process::cred
	child->kthread = false;
	for (int i = 0; i < (int) sizeof child->comm; i++)
		child->comm[i] = parent->comm[i];      // inherit name until the child exec's
	for (int i = 0; i < (int) sizeof child->cmdline; i++)
		child->cmdline[i] = parent->cmdline[i];
	child->pgid = parent->pgid;                // inherit the process group + session
	child->sid = parent->sid;
	// POSIX: fork in a multithreaded process duplicates ONLY the calling thread — the child
	// gets a single leader thread that is a copy of whichever parent thread issued fork(), NOT
	// necessarily the parent's leader. So the per-thread state (block mask, TLS base) is inherited
	// from ProcTable::currentThread() (the parent thread now running this syscall), not the leader.
	Thread* callerThread = ProcTable::currentThread();
	sigForkInherit(child->psig, parent->psig);                            // dispositions (process-wide)
	sigForkInherit(child->leaderThread()->sig, callerThread->sig);        // block mask (calling thread)
	// Inherit the TLS base: mmuCopyAddressSpace duplicated the calling thread's TLS block (the
	// musl/picolibc TCB) at the SAME virtual address, so the child's %gs:0 must point at it too.
	// Without this the child runs with GDT TLS base 0 and the first errno/__thread access (now
	// %gs-relative since the per-thread-errno migration) faults — fatal for a fork-heavy program
	// like bash. The child's own crt0 re-runs set_thread_area after exec; this keeps TLS valid in
	// the pre-exec window and in fork-without-exec children (subshells, pipelines, command
	// substitution). A non-leader thread that forks gets ITS OWN TLS, not the leader's.
	child->leaderThread()->tlsBase = callerThread->tlsBase;

	Task* t = Scheduler::createBlank(child->pid);   // allocates the child's kernel stack (heap)
	if (!t) {                                   // out of memory / task table full: fail cleanly,
		delete child->sys;                      // undoing everything we allocated, so the caller
		unsigned cur = arch::mmuCurrentDirPhys();   // sees -EAGAIN instead of a corrupt child
		arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());
		arch::mmuFreeAddressSpace((arch::AddressSpace*) space);
		arch::mmuLoadDirPhys(cur);
		ProcTable::freeSlot(child);             // releases the process slot AND its leader thread slot
		return -11;                             // -EAGAIN (Linux: fork hits the memory ceiling)
	}
	ProcTable::bindTask(child, t, child->leaderThread());   // wire task<->process<->leader thread in one place
	arch::archForkChild(t, tf, arch::mmuSpaceDirPhys(space));
	t->state = TASK_READY;                      // scheduler picks it up; resumes with eax=0
	return child->pid;                          // parent sees the child's pid
}

// clone(2) — thread creation (the pthread keystone). Unlike fork, a CLONE_THREAD clone adds a
// new Task+Thread to the CURRENT process (NO ProcTable::alloc): it shares the address space and
// fd table, and runs on its own user stack. The new thread resumes from the same trap frame with
// eax=0 (clone returns 0 in the child) on `childStack`. Returns the new tid, or <0.
int cloneThread(arch::TrapFrame* tf, unsigned flags, unsigned childStack,
		unsigned ptid, unsigned tls, unsigned ctid) {
	// Not a thread-group clone? A no-CLONE_VM clone is a plain fork (glibc/musl fork() is
	// clone(SIGCHLD,...)) — delegate so the existing, well-tested fork path owns it. Any other
	// shape (e.g. CLONE_VM without CLONE_THREAD: vfork) is not supported here.
	if (!cloneIsThread(flags)) {
		if (!cloneSharesAddressSpace(flags))
			return forkProcess(tf);
		return -22;   // -EINVAL: unsupported clone shape (we only do fork or full thread)
	}

	Process* p = ProcTable::current();
	Thread* th = ProcTable::allocThread(p);     // fresh tid, linked into p->threads, threadCount++
	if (!th)
		return -11;   // -EAGAIN: thread pool exhausted
	Task* t = Scheduler::createBlank(th->tid);  // a task slot + kernel stack (kesp fabricated below)
	if (!t) {
		ProcTable::freeThread(th);              // undo the thread slot so we leak nothing
		return -11;   // -EAGAIN: task table / kstack memory exhausted
	}

	// Wire the task<->thread links DIRECTLY — deliberately NOT ProcTable::bindTask, because
	// bindTask also sets p->task = t, which would hijack the process's PRIMARY task (the leader's
	// scheduler context that waitpid/SIGCHLD route through). A clone child is a SECONDARY task of
	// the same process; only its own thread points back at it.
	t->proc = p;
	t->thread = th;
	th->task = t;

	// CLONE_SETTLS: record the child's TLS base so the scheduler installs it (archLoadThreadTls)
	// on first switch, and __thread / errno resolve to the child's own block. The `tls` argument's
	// meaning is ABI-specific: on i386 it points at a user_desc (same struct as set_thread_area),
	// whose base_addr is the block; on x86_64 it IS the base value directly (the %fs.base the
	// child's thread pointer needs — musl passes the TCB address, no user_desc).
	if ((flags & CLONE_SETTLS) && tls) {
#if defined(__x86_64__)
		th->tlsBase = tls;
#else
		UserDesc* ud = (UserDesc*) tls;
		th->tlsBase = ud->base_addr;
#endif
	}
	// CLONE_PARENT_SETTID: publish the new tid to the PARENT's *ptid (shared space -> visible now).
	if ((flags & CLONE_PARENT_SETTID) && ptid)
		*(int*) ptid = th->tid;
	// CLONE_CHILD_SETTID: publish the new tid to *ctid (the child would also write it, but the
	// shared address space lets us do it here once — Linux writes it from the child's context).
	if ((flags & CLONE_CHILD_SETTID) && ctid)
		*(int*) ctid = th->tid;
	// CLONE_CHILD_CLEARTID: on this thread's exit, zero *ctid and futex-wake it — the kernel half
	// of the pthread_join handshake. Record the address now; procThreadExit fires it.
	if (flags & CLONE_CHILD_CLEARTID)
		th->clearTidAddr = ctid;

	// Fabricate the child's kernel stack: same as fork, but it runs on childStack and shares the
	// parent's (current) page directory — a thread gets no private address space.
	arch::archCloneChild(t, tf, arch::mmuSpaceDirPhys((arch::AddressSpace*) p->space), childStack);
	t->state = TASK_READY;                      // scheduler picks it up; resumes with eax=0
	return th->tid;                             // the caller sees the new thread's tid
}

// When a process exits it may orphan one of its children's process groups (POSIX): a group
// that loses its last live, in-session, out-of-group parent. If such a group has stopped
// members, they must receive SIGHUP then SIGCONT so they are not left blocked forever.
// Call AFTER marking the dying process exited, so the orphan test sees it as gone.
static void orphanCheckOnExit(Process* dying) {
	// Snapshot on the heap, not the kernel stack: the table holds up to ProcTable::MAX entries.
	ProcInfo* arr = (ProcInfo*) malloc(sizeof(ProcInfo) * ProcTable::MAX);
	if (!arr)
		return;                       // OOM: skip best-effort orphan handling (non-fatal)
	int* done = (int*) malloc(sizeof(int) * ProcTable::MAX);
	if (!done) { free(arr); return; }
	int n = ProcTable::snapshot(arr, ProcTable::MAX);
	int nd = 0;
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
	free(arr); free(done);
}

// SYS_exit tail: free the address space we are standing on (after switching to the
// kernel directory so we never free the live CR3), zombify the task, schedule away.
void procExit() {
	Process* p = ProcTable::current();
	p->exitCode = p->sys->code();
	p->exited = true;
	p->sys->closeAll();      // drop fd/pipe refcounts NOW so peers (e.g. a window server) see
	                         // EOF at exit, not only when the parent reaps this zombie
	orphanCheckOnExit(p);    // re-parent fallout: SIGHUP+SIGCONT any newly-orphaned stopped group
	// POSIX: our children are now orphans — re-home them on init (pid 1) so they stay reapable
	// (their pid would otherwise name a dead parent forever, leaking the slot + 8 KB stack).
	// Nudge init in case any are already zombies waiting to be collected.
	if (p->pid != 1 && ProcTable::reparentChildren(p->pid, 1) > 0) {
		Process* init = ProcTable::byPid(1);
		if (init) { sigPost(init->psig, SIGCHLD); Scheduler::wake(init->task); }
	}
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());
	if (p->space) {
		arch::mmuFreeAddressSpace((arch::AddressSpace*) p->space);
		p->space = 0;
	}
	// Notify our parent of our death (SIGCHLD + wake) so a parent blocked in waitpid — or one
	// sitting in its SIGCHLD handler's read(), like the init shell collecting orphans — reaps us.
	Process* parent = ProcTable::byPid(p->parent);
	if (parent) {
		sigPost(parent->psig, SIGCHLD);
		Scheduler::wake(parent->task);
	}
	Scheduler::current()->state = TASK_ZOMBIE;
	Scheduler::schedule();   // never returns to this (now zombie) task
	for (;;) {}              // unreachable
}

// Per-thread exit: a NON-LAST thread of a multithreaded process called SYS_exit. We tear down
// only THIS thread — the address space, fd table and surviving threads stay alive. Does NOT
// return. (The last thread takes the normal procExit path instead; see the SYS_exit dispatch.)
void procThreadExit(int code) {
	(void) code;   // a thread carries no waitpid-reportable status; pthread_join learns of
	               // completion via the CLEARTID futex below, not an exit code.
	Process* p = ProcTable::current();
	Thread* th = ProcTable::currentThread();
	unsigned ctid = th ? th->clearTidAddr : 0;

	// CLONE_CHILD_CLEARTID handshake: zero the tid word and wake one joiner blocked in
	// futex(FUTEX_WAIT) on it (the pthread_join side). The address space is shared, so the
	// store is immediately visible to the joiner.
	if (ctid) {
		*(int*) ctid = 0;
		futexWakeAddr(p->space, (void*) ctid, 1);
	}

	Task* self = Scheduler::current();
	if (th)
		ProcTable::freeThread(th);   // release the thread slot + threadCount-- (unlinks from p->threads)
	// If the LEADER thread exits first (POSIX allows pthread_exit() from main while workers run),
	// its task is the process's primary p->task — the slot waitProcess reaps when the group later
	// ends. Reaping self would leave p->task dangling at a freed/reused slot (a later procExit
	// would reap the wrong task). Repoint p->task at a surviving thread's task, mirroring
	// procExitGroup. (p->threads now excludes self, since freeThread unlinked it.)
	if (p->task == self && p->threads && p->threads->task)
		p->task = p->threads->task;
	// Mark the task DONE, not ZOMBIE: a thread is never reaped via waitpid, so the scheduler's
	// onTick reclaims a DONE task's slot + kstack on its own (the ZOMBIE convention exists only
	// for a process a parent must wait on). The user stack + TLS belong to userland (pthread
	// join/detach owns them), so the kernel leaves them untouched.
	self->state = TASK_DONE;
	Scheduler::schedule();   // never returns to this (now done) task
	for (;;) {}              // unreachable
}

// exit_group(2): terminate the WHOLE thread group. Reap every SIBLING thread's task (so none
// lingers runnable), then run the normal process teardown — which frees the shared address
// space, taking all threads' user stacks with it. Does NOT return. For a single-threaded
// process this is exactly equivalent to a plain exit (no siblings to reap).
void procExitGroup(int code) {
	Process* p = ProcTable::current();
	Task* self = Scheduler::current();
	// Reap each sibling task's kernel stack/slot. We do NOT freeThread here: ProcTable::freeSlot
	// (run when the parent reaps this process) drains the whole thread list, so we only need the
	// scheduler Task slots gone now. A sibling parked on a futex has its FutexWaiter node ON the
	// kstack we are about to free; evict it from the futex table first (Task 6.1 hardening) so the
	// bucket never holds a pointer into freed memory. (Harmless in practice — the whole group is
	// dying so nothing would wake it — but it keeps the table strictly consistent.)
	for (Thread* th = p->threads; th; th = th->next)
		if (th->task && th->task != self) {
			futexRemoveTask(p->space, th->task);
			Scheduler::reap(th->task);
		}
	// The calling thread becomes the process's zombie task the parent reaps. If the leader's task
	// was among those reaped above, repoint p->task at the survivor so waitpid reaps a live slot.
	p->task = self;
	p->sys->exit(code);   // record the group exit status (procExit reads p->sys->code())
	procExit();           // frees the shared address space + zombifies self + wakes parent; no return
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

namespace { SigMask sigbit(int s) { return (SigMask) 1 << (s - 1); } }   // 64-bit mask bit

// Terminate the current process because of a fatal signal. Mirrors procExit but records
// the killing signal (so waitpid reports WIFSIGNALED). Does NOT return.
static void procKill(int sig) {
	Process* p = ProcTable::current();
	p->termSignal = sig;
	p->exitCode = sig;
	p->exited = true;
	p->sys->closeAll();      // release fds/pipes at death so peers see EOF before the reap
	orphanCheckOnExit(p);    // same orphan-group handling as a normal exit
	// Re-home our children on init (pid 1) so they stay reapable after we die — identical to
	// procExit. A signal death must do this too, or grandchildren name a freed parent slot and
	// leak as unreapable zombies.
	if (p->pid != 1 && ProcTable::reparentChildren(p->pid, 1) > 0) {
		Process* init = ProcTable::byPid(1);
		if (init) { sigPost(init->psig, SIGCHLD); Scheduler::wake(init->task); }
	}
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());
	if (p->space) {
		arch::mmuFreeAddressSpace((arch::AddressSpace*) p->space);
		p->space = 0;
	}
	// Notify the parent with SIGCHLD (+ wake), exactly as procExit does — a parent collecting
	// children asynchronously via a SIGCHLD handler (e.g. a job-control shell) must learn of a
	// signal death, not only a normal exit.
	Process* parent = ProcTable::byPid(p->parent);
	if (parent) {
		sigPost(parent->psig, SIGCHLD);
		Scheduler::wake(parent->task);
	}
	Scheduler::current()->state = TASK_ZOMBIE;
	Scheduler::schedule();
	for (;;) {}   // unreachable
}

// Public entry for the CPU-exception backstop (arch fault handler): a ring-3 fault terminates the
// faulting process like a fatal signal, leaving the rest of the system running. Does NOT return.
void killCurrentProcess(int sig) { procKill(sig); }

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
		sigPost(parent->psig, SIGCHLD);
		Scheduler::wake(parent->task);
	}
	Scheduler::current()->state = TASK_STOPPED;
	Scheduler::schedule();         // resumes here once continued (or killed)
	p->stopped = false;
}

// Broadcast (kill(-1)) helper: visited once per live process by ProcTable::forEachLive, so the
// whole table is signalled without allocating a snapshot buffer on the heap.
struct BcastCtx { int sig; int self; int rc; };
static void bcastVisit(int pid, bool kthread, void* c) {
	BcastCtx* b = (BcastCtx*) c;
	if (pid == 1 || pid == b->self || kthread)
		return;                       // never signal init, ourselves, or a kernel thread
	if (signalSend(pid, b->sig) == 0)
		b->rc = 0;
}

// Walk a process's intrusive thread list and pick which thread should take a process-directed
// signal. This is the SAME 3-case policy the pure (host-tested) pickSignalTarget encodes (see
// kernel/Signal.cpp), applied directly to the list so it is unbounded and copy-free: a thread
// group can hold up to ProcTable::MAX threads, far too many for an on-stack mask array (the
// kernel stack is only 8 KB), and this can run from the keyboard IRQ via consoleSignal ->
// signalSend, so no heap either. The leader is the head of the list (p->threads).
static Thread* pickSignalTargetThread(Process* p, int sig) {
	if (!p || !p->threads)
		return 0;
	// SIGKILL/SIGSTOP are immune to the block mask: the leader takes them.
	if (sig == SIGKILL || sig == SIGSTOP)
		return p->threads;
	// First thread that does not block the signal (leader preferred, it is the head).
	for (Thread* th = p->threads; th; th = th->next)
		if (!th->sig.isBlocked(sig))
			return th;
	return p->threads;   // every thread blocks it -> leave it pending on the leader
}

// kill(2): post `sig` to process `pid`. Wakes a blocked target so it can deliver. Per POSIX
// a non-positive pid targets a process group: pid == 0 is the caller's group, pid < 0 is
// the group |pid|.
int signalSend(int pid, int sig) {
	if (sig < 0 || sig >= NANOS_NSIG)
		return -22;   // -EINVAL
	if (sig == SIGCANCEL)
		return -22;   // -EINVAL: SIGCANCEL is kernel-internal (pthread_cancel); apps can't send it
	if (pid == -1) {  // broadcast: every process we may signal, except init (pid 1) and self
		Process* me = ProcTable::current();
		BcastCtx bc = { sig, me ? me->pid : 0, -3 };
		ProcTable::forEachLive(bcastVisit, &bc);   // snapshot-free: no heap allocation
		return bc.rc;
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
	if (t->exited)
		return 0;     // signalling a zombie is a no-op (its sibling Tasks may already be freed)
	sigPost(t->psig, sig);
	if (sig == SIGCONT && t->stopped) {        // resume a stopped process (job control)
		t->stopped = false;
		t->continued = true;
		Process* parent = ProcTable::byPid(t->parent);
		if (parent) { sigPost(parent->psig, SIGCHLD); Scheduler::wake(parent->task); }
	}
	// Process-directed: wake a thread that does NOT block the signal so it reaches a
	// return-to-user and delivers (Linux routes kill(pid) to any able thread). With one thread
	// the leader is the only candidate -> identical to the old single-thread behaviour.
	Thread* target = pickSignalTargetThread(t, sig);
	Task* tk = target ? target->task : t->task;
	if (tk) {
		if (tk->state == TASK_BLOCKED)
			Scheduler::wake(tk);
		else if (tk->state == TASK_STOPPED && (sig == SIGCONT || sig == SIGKILL))
			Scheduler::resume(tk);        // STOPPED -> READY: only a continue/kill un-stops it
	}
	return 0;
}

// tgkill(2)/tkill(2): post `sig` to the SPECIFIC thread `tid` (thread-directed pending, which
// signalDeliver folds into that thread's deliverable set when it returns to user) and wake it.
// SIGCANCEL is ALLOWED here — this is the in-process pthread_cancel transport (Task 5.3); only
// kill(2)-by-pid refuses it. tgid >= 0 requires the thread to be in that thread group.
int signalSendThread(int tgid, int tid, int sig) {
	if (sig < 0 || sig >= NANOS_NSIG)
		return -22;   // -EINVAL
	Thread* th = ProcTable::threadByTid(tid);
	if (!th || !th->proc)
		return -3;    // -ESRCH: no such thread
	Process* t = th->proc;
	if (tgid >= 0 && t->tgid != tgid)
		return -3;    // -ESRCH: thread is not in the named thread group
	if (sig == 0)
		return 0;     // existence/permission check only
	if (t->exited)
		return 0;     // signalling a zombie is a no-op (its sibling Tasks may already be freed)
	sigPost(th->sig, sig);                     // thread-directed: lands on THIS thread's pending
	if (sig == SIGCONT && t->stopped) {        // job-control stop/continue is per-process
		t->stopped = false;
		t->continued = true;
		Process* parent = ProcTable::byPid(t->parent);
		if (parent) { sigPost(parent->psig, SIGCHLD); Scheduler::wake(parent->task); }
	}
	// Wake the targeted thread's own task so it reaches a return-to-user and delivers.
	if (th->task) {
		if (th->task->state == TASK_BLOCKED)
			Scheduler::wake(th->task);
		else if (th->task->state == TASK_STOPPED && (sig == SIGCONT || sig == SIGKILL))
			Scheduler::resume(th->task);
	}
	return 0;
}

// signal(2): install a disposition for the current process. `handler` is kSigDefault,
// kSigIgnore, or a user function address; `restorer` (the libc sigreturn trampoline) is
// remembered when nonzero. Returns the previous disposition.
int signalAction(int sig, unsigned handler, unsigned restorer) {
	if (sig <= 0 || sig >= NANOS_NSIG || !sigCanCatch(sig) || sig == SIGCANCEL)
		return -22;   // -EINVAL (SIGCANCEL is kernel-internal; apps can't install a disposition)
	Process* p = ProcTable::current();
	unsigned prev = p->psig.handlers[sig];
	if (handler == 0xFFFFFFFFu)
		return (int) prev;   // query only (sigaction with act == NULL): don't change anything
	p->psig.handlers[sig] = handler;
	if (restorer)
		p->psig.restorer = restorer;
	// signal() carries SA_RESTART (glibc/BSD semantics): a handler restarts interrupted
	// syscalls. Default/ignore dispositions clear it.
	if (handler != kSigDefault && handler != kSigIgnore)
		p->psig.restart |= sigbit(sig);
	else
		p->psig.restart &= ~sigbit(sig);
	return (int) prev;
}

// sigprocmask(2) — LEGACY single-word form (SYS_sigprocmask). how 0=BLOCK, 1=UNBLOCK,
// 2=SETMASK. SIGKILL/SIGSTOP stay unblockable. The blocked mask is now 64-bit, but this
// legacy call only addresses signals 1..31: BLOCK/UNBLOCK OR/AND-NOT the low word, and
// SETMASK replaces ONLY the low 32 bits — the high bits (signals 32..64, owned by the rt_*
// callers and the kernel's SIGCANCEL) must never be clobbered by a legacy mask.
int signalMask(int how, unsigned set, unsigned* oldset) {
	Thread* th = ProcTable::currentThread();   // sigprocmask is per-thread
	if (oldset)
		*oldset = (unsigned) th->sig.blocked;   // low word only (legacy sigset is one word)
	switch (how) {
	case 0: th->sig.blocked |= (SigMask) set; break;
	case 1: th->sig.blocked &= ~(SigMask) set; break;
	case 2: th->sig.blocked = (th->sig.blocked & 0xFFFFFFFF00000000ull) | set; break;
	default: return -22;   // -EINVAL
	}
	th->sig.blocked &= ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
	return 0;
}

// rt_sigprocmask(2): the wide form. The mask is carried by pointer, so signals 32..64 are
// addressable; sigsetsize MUST be 8 (a 64-bit mask). Same how semantics as the legacy call,
// but operating on the full 64-bit blocked set.
int signalMaskRt(int how, const uint64_t* set, uint64_t* oldset, unsigned sigsetsize) {
	if (sigsetsize != 8)
		return -22;   // -EINVAL: NanOS only supports a 64-bit sigset
	Thread* th = ProcTable::currentThread();
	if (oldset)
		*oldset = th->sig.blocked;
	if (set) {
		switch (how) {
		case 0: th->sig.blocked |= *set; break;
		case 1: th->sig.blocked &= ~*set; break;
		case 2: th->sig.blocked = *set; break;
		default: return -22;   // -EINVAL
		}
		th->sig.blocked &= ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
	}
	return 0;
}

// rt_sigaction(2): the wide form, reading/writing the kernel-ABI struct k_sigaction (the
// layout musl marshals into). NanOS's signal model only honours the handler + restorer (it
// implies SA_RESTART and runs handlers with the delivered signal blocked), so sa_flags and
// sa_mask beyond that are accepted but not separately applied — consistent with signal()/
// signalAction().
//
// UNLIKE signal()/signalAction() and kill()-by-pid (which both refuse SIGCANCEL as
// kernel-internal), rt_sigaction is the ONE path that ALLOWS installing a handler for
// SIGCANCEL. This is deliberate: the pthread runtime owns SIGCANCEL (it is the in-process
// pthread_cancel transport, Task 5.3) and must install a no-op handler so the signal's
// disposition becomes "run handler / interrupt the syscall" rather than the RT-signal default
// of terminating the thread. The kernel cannot tell the pthread runtime apart from an app, and
// a modern threaded libc legitimately owns this signal — this matches the Linux convention.
// To make a blocked cancellable futex actually return -EINTR (so __syscall_cp can act on the
// pending cancel) rather than RESTART, SIGCANCEL is FORCED non-restarting here regardless of
// the requested flags — otherwise cancellation of a thread parked in futex() would deadlock.
int signalActionRt(int sig, const k_sigaction* act, k_sigaction* old, unsigned sigsetsize) {
	if (sigsetsize != 8)
		return -22;   // -EINVAL
	if (sig <= 0 || sig >= NANOS_NSIG || !sigCanCatch(sig))
		return -22;   // -EINVAL
	Process* p = ProcTable::current();
	unsigned prev = p->psig.handlers[sig];
	if (act) {
		unsigned handler  = (unsigned) (unsigned long) act->k_sa_handler;
		unsigned restorer = (unsigned) (unsigned long) act->k_sa_restorer;
		p->psig.handlers[sig] = handler;
		if (restorer)
			p->psig.restorer = restorer;
		if (handler != kSigDefault && handler != kSigIgnore)
			p->psig.restart |= sigbit(sig);
		else
			p->psig.restart &= ~sigbit(sig);
		// SIGCANCEL must never restart an interrupted syscall: cancellation relies on the
		// cancellable futex returning -EINTR so __syscall_cp can run __cancel(). Force the
		// restart bit clear no matter what flags userland asked for.
		if (sig == SIGCANCEL)
			p->psig.restart &= ~sigbit(sig);
	}
	if (old) {
		old->k_sa_handler  = (void*) (unsigned long) prev;
		old->k_sa_flags    = 0;
		old->k_sa_restorer = 0;
		old->k_sa_mask[0]  = 0;
		old->k_sa_mask[1]  = 0;
	}
	return 0;
}

// rt_sigpending(2): report the set of pending signals for the caller — the union of this
// thread's pending and the process-directed pending (all pending, not just the blocked ones).
int signalPendingRt(uint64_t* set, unsigned sigsetsize) {
	if (sigsetsize != 8)
		return -22;   // -EINVAL
	if (set) {
		Process* p = ProcTable::current();
		Thread* th = ProcTable::currentThread();
		*set = th->sig.pending | p->psig.pending;
	}
	return 0;
}

// pause(2): block until a signal is delivered, then always return -EINTR (pause never
// restarts, even under SA_RESTART — so we hand back -EINTR directly rather than the
// -ERESTARTSYS sentinel). Scheduler::block() returns at once if a signal is already
// pending, and signalSend wakes a BLOCKED target, so the loop unblocks exactly on a
// deliverable signal.
int signalPause() {
	while (!hasPendingSignalCurrent())
		Scheduler::block();
	return -4;   // -EINTR
}

// sigsuspend(2): atomically install `mask` as the blocked set, wait for a signal that mask
// leaves deliverable, then restore the previous mask and return -EINTR. NOTE: we restore the
// old mask before the handler runs at the return-to-user point (not after, as Linux does via
// the saved-mask sigreturn path). With NanOS's single-word sigset and the simplified handler
// model this is behaviourally identical for the sigsuspend idiom (wait-for-one-signal-then-
// loop, e.g. inetd's reconfigure pause) — the handler runs once and the caller re-tests its
// condition either way. Documented here as a known, bounded simplification, consistent with
// the kernel's existing signal fidelity (see signalAction's sa_mask note).
int signalSuspend(unsigned mask) {
	Thread* th = ProcTable::currentThread();
	SigMask old = th->sig.blocked;
	// Legacy single-word form: set the low 32 bits from `mask`, keep the high bits (32..64)
	// as they were — the wait-mask only addresses signals 1..31.
	th->sig.blocked = ((old & 0xFFFFFFFF00000000ull) | mask)
	                & ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
	while (!hasPendingSignalCurrent())
		Scheduler::block();
	th->sig.blocked = old;
	return -4;   // -EINTR
}

// rt_sigsuspend(2): the wide form. Installs the full 64-bit `mask` as the blocked set, waits
// for a deliverable signal, then restores the previous mask. sigsetsize MUST be 8.
int signalSuspendRt(const uint64_t* mask, unsigned sigsetsize) {
	if (sigsetsize != 8)
		return -22;   // -EINVAL
	Thread* th = ProcTable::currentThread();
	SigMask old = th->sig.blocked;
	th->sig.blocked = (mask ? *mask : 0) & ~(sigbit(SIGKILL) | sigbit(SIGSTOP));
	while (!hasPendingSignalCurrent())
		Scheduler::block();
	th->sig.blocked = old;
	return -4;   // -EINTR
}

bool hasPendingSignalCurrent() {
	Process* p = ProcTable::current();
	Thread* th = ProcTable::currentThread();
	// ignored signals (SIGCHLD) must not cause EINTR
	return p && th && sigHasInterrupt(th->sig, p->psig);
}

// Deliver pending signals at a return-to-user point. The caller has already ensured the
// trap frame returns to ring 3.
void signalDeliver(arch::TrapFrame* tf, unsigned origEax, bool inSyscall) {
	Process* p = ProcTable::current();
	Thread* th = ProcTable::currentThread();
	if (!p || !th || p->kthread)
		return;
	// Was this a blocking syscall interrupted by a signal (eligible for restart/EINTR)?
	bool restartable = inSyscall && (arch::archSyscallResult(tf) == -ERESTARTSYS);
	for (;;) {
		int sig = sigNextDeliverable(th->sig, p->psig);
		if (!sig)
			break;
		sigConsume(th->sig, p->psig, sig);
		switch (sigResolve(p->psig, sig)) {
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
				action = (p->psig.restart & sigbit(sig)) ? arch::SIG_FRAME_RESTART
				                                         : arch::SIG_FRAME_EINTR;
			SigMask oldMask = th->sig.blocked;
			th->sig.blocked |= sigbit(sig);
			arch::archPushSignalFrame(tf, p->psig.handlers[sig], p->psig.restorer,
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
	uint64_t oldMask = 0;   // archSigreturn restores the full 64-bit mask (signals 1..64)
	int rc = arch::archSigreturn(tf, &oldMask);
	ProcTable::currentThread()->sig.blocked = oldMask;
	return rc;
}

// A control key from the cooked-mode tty (Ctrl+C/Ctrl+\/Ctrl+Z) -> deliver `sig` to the
// foreground process (the child the shell is blocked on in waitpid).
void consoleSignal(int sig) {
	// Job control active (a shell ran tcsetpgrp on the console): deliver to the whole
	// foreground process group, like a real tty. Otherwise fall back to the single pid the
	// shell is waiting on (nsh, which never claims the terminal).
	if (g_consolePgrp > 0) {
		signalSendGroup(g_consolePgrp, sig);
		return;
	}
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
	// Enumerate group members into a small ON-STACK buffer. This runs from the keyboard IRQ
	// (Ctrl+C/Z -> consoleSignal), and the kernel heap is NOT reentrant against an interrupt,
	// so there must be no malloc on this path. A terminal process group never approaches this
	// many members; a larger one would have its tail dropped (acceptable for signal delivery).
	int pids[64];
	int n = ProcTable::groupMembers(pgid, pids, 64);
	if (n == 0)
		return -3;
	int rc = -3;
	for (int i = 0; i < n; i++) {
		Process* p = ProcTable::byPid(pids[i]);
		if (!p || p->kthread)
			continue;
		sigPost(p->psig, sig);
		if (sig == SIGCONT && p->stopped) {
			p->stopped = false;
			p->continued = true;
			Process* par = ProcTable::byPid(p->parent);
			if (par) { sigPost(par->psig, SIGCHLD); Scheduler::wake(par->task); }
		}
		if (p->task) {
			if (p->task->state == TASK_BLOCKED)
				Scheduler::wake(p->task);
			else if (p->task->state == TASK_STOPPED && (sig == SIGCONT || sig == SIGKILL))
				Scheduler::resume(p->task);   // STOPPED -> READY: only a continue/kill un-stops it
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

// ---- ITIMER_REAL interval timer (setitimer/getitimer) --------------------------------
// The state lives on the process (Process::itReal); the scheduler tick decrements it and
// raises SIGALRM (ProcTable::tickRealTimers). These two just marshal the kernel-ABI
// timeval pairs to/from the microsecond counters. Only ITIMER_REAL (0) is implemented.
static uint64_t tvToUs(long sec, long usec) {
	if (sec < 0) sec = 0;
	if (usec < 0) usec = 0;
	return (uint64_t) sec * 1000000ull + (uint64_t) usec;
}
static void usToTv(uint64_t us, long* sec, long* usec) {
	*sec  = (long) (us / 1000000ull);
	*usec = (long) (us % 1000000ull);
}

int sysGetitimer(int which, ::k_itimerval* oval) {
	if (which != K_ITIMER_REAL)
		return -22;   // -EINVAL: VIRTUAL/PROF not implemented
	if (!oval)
		return -14;   // -EFAULT
	Process* p = ProcTable::current();
	if (!p)
		return -3;    // -ESRCH (should not happen from a user context)
	usToTv(p->itReal.valueUs,    &oval->it_value_sec,    &oval->it_value_usec);
	usToTv(p->itReal.intervalUs, &oval->it_interval_sec, &oval->it_interval_usec);
	return 0;
}

int sysSetitimer(int which, const ::k_itimerval* nval, ::k_itimerval* oval) {
	if (which != K_ITIMER_REAL)
		return -22;   // -EINVAL
	Process* p = ProcTable::current();
	if (!p)
		return -3;
	if (oval) {       // report the PREVIOUS setting before overwriting (Linux semantics)
		usToTv(p->itReal.valueUs,    &oval->it_value_sec,    &oval->it_value_usec);
		usToTv(p->itReal.intervalUs, &oval->it_interval_sec, &oval->it_interval_usec);
	}
	if (nval) {       // a NULL new value queries only (old is still reported above)
		p->itReal.valueUs    = tvToUs(nval->it_value_sec,    nval->it_value_usec);
		p->itReal.intervalUs = tvToUs(nval->it_interval_sec, nval->it_interval_usec);
		// it_value == 0 disarms the timer (valueUs 0); tickRealTimers then skips it.
	}
	return 0;
}

}
