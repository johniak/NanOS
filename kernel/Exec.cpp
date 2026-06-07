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
	kernelSyscalls()->resetForRun();

	arch::archFrameToUser(tf, entry, esp);   // iret will enter the new program ...
	arch::mmuSwitch(newSpace);               // ... under the new address space.
	return 0;                                // value irrelevant (frame rewritten)
}

// SYS_exit tail: free the address space we are standing on (after switching to the
// kernel directory so we never free the live CR3), zombify the task, schedule away.
void procExit() {
	Process* p = ProcTable::current();
	p->exitCode = p->sys->code();
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());
	if (p->space) {
		arch::mmuFreeAddressSpace((arch::AddressSpace*) p->space);
		p->space = 0;
	}
	Scheduler::current()->state = TASK_ZOMBIE;
	Scheduler::schedule();   // never returns to this (now zombie) task
	for (;;) {}              // unreachable
}

}
