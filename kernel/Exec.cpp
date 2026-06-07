#include "Exec.h"
#include "NxeLoader.h"
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "String.h"
#include <arch/usermode.h>

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
	return arch::enterUser(entry, 0x500000, 0);   // ring 0 for now (step B2)
}

}
