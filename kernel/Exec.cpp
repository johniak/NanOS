#include "Exec.h"
#include "NxeLoader.h"
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "String.h"
#include <arch/usermode.h>
#include <arch/mmu.h>

namespace kernel {

// Load a .nxe program and run it. Machine-independent: read the image via the
// VFS into the staging window, validate + zero bss, then hand the entry point to
// the arch (which runs it and returns the exit code). The program talks to the
// kernel via int 0x80; the IAT import path is gone (importCount == 0).
int execProgram(Vfs* vfs, const char* path) {
	String p = String((char*) path);
	FileStat st;
	if (vfs->stat(p, st) < 0)
		return -1;

	// Stage the image at the fixed load base (kernel identity window).
	char* image = (char*) 0x400000;
	if (vfs->read(p, st.size, 0, image) < 0)
		return -1;

	NxHeader* h = (NxHeader*) image;
	unsigned span = h->bssEnd - h->loadBase;

	unsigned entry = 0;
	int rc = NxeLoader::loadImage(image, span, 0, &entry);   // 0 imports -> no resolver
	if (rc < 0)
		return rc;

	kernelSyscalls()->resetForRun();
	// Run the program in ring 3 in its own address space. argv[0] = the path.
	const char* argv[] = { path, 0 };
	return arch::execUserImage(entry, h->loadBase, h->bssEnd, argv, 1);
}

int spawnProgram(Vfs* vfs, const char* path, const char* const* argv, int argc) {
	// Called from a syscall while the PARENT runs in ring 3 (its space is active).
	// Stage the child image under the kernel directory so writing the staging
	// window (0x400000) does not corrupt the parent's private page mapped there.
	// argv is already copied into kernel memory by the caller (readable under any
	// directory), so swapping CR3 here is safe.
	uint32_t parentDir = arch::mmuCurrentDirPhys();
	arch::mmuLoadDirPhys(arch::mmuKernelDirPhys());

	int result;
	String p = String((char*) path);
	FileStat st;
	if (vfs->stat(p, st) < 0) {
		result = -2;   // -ENOENT
	} else {
		char* image = (char*) 0x400000;
		if (vfs->read(p, st.size, 0, image) < 0) {
			result = -1;
		} else {
			NxHeader* h = (NxHeader*) image;
			unsigned entry = 0;
			int rc = NxeLoader::loadImage(image, h->bssEnd - h->loadBase, 0, &entry);
			if (rc < 0)
				result = rc;
			else
				result = arch::spawnUserImage(entry, h->loadBase, h->bssEnd, argv, argc);
		}
	}

	arch::mmuLoadDirPhys(parentDir);   // back to the parent's space before returning
	return result;
}

}
