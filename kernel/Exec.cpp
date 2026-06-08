#include "Exec.h"
#include "NxeLoader.h"
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "Process.h"
#include "Scheduler.h"
#include "String.h"
#include <arch/usermode.h>
#include <arch/mmu.h>
#include <arch/sched.h>

namespace kernel {

// Load a .nxe image (already staged at the load base in the kernel identity window),
// validating + zeroing bss. Returns the entry point, or <0 on error. Caller must be
// on a directory where the staging window 0x400000 is identity-mapped.
static int loadStaged(unsigned* entryOut) {
	char* image = (char*) 0x400000;
	NxHeader* h = (NxHeader*) image;
	unsigned span = h->bssEnd - h->loadBase;
	return NxeLoader::loadImage(image, span, 0, entryOut);   // 0 imports -> no resolver
}

// PID 1 launch (init task body, kernel directory active): stage the image, build a
// fresh address space for it, enter ring 3. Does not return on success.
int execProgram(Vfs* vfs, const char* path) {
	String p = String((char*) path);
	FileStat st;
	if (vfs->stat(p, st) < 0)
		return -1;
	char* image = (char*) 0x400000;
	if (vfs->read(p, st.size, 0, image) < 0)
		return -1;
	NxHeader* h = (NxHeader*) image;
	unsigned entry = 0;
	int rc = loadStaged(&entry);
	if (rc < 0)
		return rc;

	arch::AddressSpace* space = arch::mmuCreateAddressSpace();
	const char* argv[] = { path, 0 };
	unsigned esp = arch::archLoadUser(space, h->loadBase, h->bssEnd, argv, 1);
	ProcTable::current()->space = space;
	ProcTable::setCommand(ProcTable::current(), argv, 1);
	kernelSyscalls()->resetForRun();
	arch::archEnterUser(entry, esp, space);   // never returns
	return 0;                                 // unreachable
}

// execve(2): replace the current process's image. We run inside a syscall on the
// process's user CR3; stage + load under the kernel directory (where 0x400000 is
// identity-mapped), then rewrite the trap frame so the iret enters the new image.
int execve(Vfs* vfs, const char* path, const char* const* argv, int argc,
		arch::TrapFrame* tf) {
	Process* p = ProcTable::current();
	unsigned userDir = arch::mmuCurrentDirPhys();
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());

	String pp = String((char*) path);
	FileStat st;
	if (vfs->stat(pp, st) < 0) {
		arch::mmuLoadDirPhys(userDir);
		return -2;   // -ENOENT
	}
	char* image = (char*) 0x400000;
	if (vfs->read(pp, st.size, 0, image) < 0) {
		arch::mmuLoadDirPhys(userDir);
		return -1;
	}
	NxHeader* h = (NxHeader*) image;
	unsigned entry = 0;
	int rc = loadStaged(&entry);
	if (rc < 0) {
		arch::mmuLoadDirPhys(userDir);
		return rc;
	}

	// Load into a fresh space, then drop the caller's old image.
	arch::AddressSpace* newSpace = arch::mmuCreateAddressSpace();
	unsigned esp = arch::archLoadUser(newSpace, h->loadBase, h->bssEnd, argv, argc);
	if (p->space)
		arch::mmuFreeAddressSpace((arch::AddressSpace*) p->space);
	p->space = newSpace;
	ProcTable::setCommand(p, argv, argc);
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
	child->sys = new Syscalls(*parent->sys);   // dup the parent's fd table
	child->kthread = false;
	for (int i = 0; i < (int) sizeof child->comm; i++)
		child->comm[i] = parent->comm[i];      // inherit name until the child exec's
	for (int i = 0; i < (int) sizeof child->cmdline; i++)
		child->cmdline[i] = parent->cmdline[i];

	Task* t = Scheduler::createBlank(child->pid);
	child->task = t;
	arch::archForkChild(t, tf, arch::mmuSpaceDirPhys(space));
	t->state = TASK_READY;                      // scheduler picks it up; resumes with eax=0
	return child->pid;                          // parent sees the child's pid
}

// SYS_exit tail: free the address space we are standing on (after switching to the
// kernel directory so we never free the live CR3), zombify the task, schedule away.
void procExit() {
	Process* p = ProcTable::current();
	p->exitCode = p->sys->code();
	p->exited = true;
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

// waitpid(2): reap a child of the current process. Block until a matching child has
// exited, copy its (encoded) status to *statusOut, release its scheduler task slot
// and its syscall state, and return its pid. -ECHILD if there is no such child.
int waitProcess(int wantPid, int* statusOut) {
	Process* parent = ProcTable::current();
	for (;;) {
		Process* child = 0;
		int r = ProcTable::reapChild(parent->pid, wantPid, &child);
		if (r == -10)
			return -10;            // -ECHILD: no such child
		if (r > 0) {
			int code = child->exitCode;
			Scheduler::reap(child->task);   // free the child's task slot (kstack reuse)
			delete child->sys;              // dup'd fd table from fork
			ProcTable::freeSlot(child);     // release the process slot
			if (statusOut)
				*statusOut = (code & 0xFF) << 8;   // WEXITSTATUS-compatible encoding
			return r;
		}
		Scheduler::block();        // children alive but none exited yet: wait
	}
}

}
