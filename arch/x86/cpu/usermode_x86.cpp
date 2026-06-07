/*
 * usermode_x86.cpp — x86 implementation of <arch/usermode.h>.
 *
 * Step B2: ring-0 path only — runs the entry as a plain call; exit() traps via
 * int 0x80 -> the syscall trap detects hasExited() and calls userExit(), which
 * longjmps back into enterUser. (Ring-3 + per-process CR3 switch arrives in a
 * later step, on the `space != 0` branch.)
 */
#include <arch/usermode.h>
#include "NxJmp.h"
#include "SyscallDispatch.h"   // kernel::kernelSyscalls()

namespace {
arch::NxJmp g_userCtx;
}

namespace arch {

int enterUser(uint32_t entry, uint32_t userStackTop, AddressSpace* space) {
	(void) userStackTop;
	(void) space;   // ring-0 for now
	if (nx_setjmp(&g_userCtx) != 0)
		return kernel::kernelSyscalls()->code();   // returned here via userExit()
	((void (*)()) entry)();
	return 0;   // entry returned without exiting (shouldn't happen: crt0 calls exit)
}

void userExit() {
	nx_longjmp(&g_userCtx, 1);   // val=1; the real exit code comes from Syscalls::code()
}

}
