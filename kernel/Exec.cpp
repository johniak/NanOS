#include "Exec.h"
#include "NxeLoader.h"
#include "Syscall.h"
#include "SyscallDispatch.h"
#include "String.h"
#include <string.h>

namespace kernel {

// setjmp/longjmp context (arch/nxjmp.S).
struct NxJmp {
	unsigned esp, ebp, ebx, esi, edi, eip;
};
extern "C" int nx_setjmp(NxJmp* buf);
extern "C" void nx_longjmp(NxJmp* buf, int val);

static NxJmp g_kernelCtx;
static int g_exitCode;

// ---- Exported API: the stable named contract the loader binds imports to.
// Each call funnels into the shared Syscalls core (same fd table as int 0x80).
static int k_write(int fd, const void* b, unsigned n) {
	return kernelSyscalls()->write(fd, b, n);
}
static int k_read(int fd, void* b, unsigned n) {
	return kernelSyscalls()->read(fd, b, n);
}
static int k_open(const char* p, int f) {
	return kernelSyscalls()->open(String((char*) p), f);
}
static int k_close(int fd) {
	return kernelSyscalls()->close(fd);
}
static void k_exit(int code) {
	g_exitCode = code;
	nx_longjmp(&g_kernelCtx, 1);   // return to execProgram
}

struct Export {
	const char* name;
	void* addr;
};
static const Export g_exports[] = {
	{ "write", (void*) &k_write }, { "read", (void*) &k_read },
	{ "open", (void*) &k_open }, { "close", (void*) &k_close },
	{ "exit", (void*) &k_exit },
};

static void* resolveExport(const char* name) {
	for (unsigned i = 0; i < sizeof(g_exports) / sizeof(g_exports[0]); i++)
		if (strcmp(g_exports[i].name, name) == 0)
			return g_exports[i].addr;
	return 0;
}

int execProgram(Vfs* vfs, const char* path) {
	String p = String((char*) path);
	FileStat st;
	if (vfs->stat(p, st) < 0)
		return -1;

	// Load the image at the fixed base 0x400000.
	char* image = (char*) 0x400000;
	if (vfs->read(p, st.size, 0, image) < 0)
		return -1;

	NxHeader* h = (NxHeader*) image;
	unsigned span = h->bssEnd - h->loadBase;   // region incl. bss/IAT

	unsigned entry = 0;
	int rc = NxeLoader::loadImage(image, span, resolveExport, &entry);
	if (rc < 0)
		return rc;

	// Run it: save the kernel context, jump to entry. k_exit (or a plain return
	// from _start) brings us back here via nx_longjmp.
	g_exitCode = 0;
	if (nx_setjmp(&g_kernelCtx) == 0) {
		((void (*)()) entry)();
	}
	return g_exitCode;
}

}
