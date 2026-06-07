/*
 * NxJmp.h — setjmp/longjmp context used to return from a user program to the
 * kernel (implemented in arch/x86/cpu/nxjmp.S). x86-internal.
 */
#pragma once

namespace arch {

struct NxJmp {
	unsigned esp, ebp, ebx, esi, edi, eip;
};

// extern "C": the linker symbols are nx_setjmp/nx_longjmp (see nxjmp.S).
extern "C" int nx_setjmp(NxJmp* buf);    // returns 0 here, `val` when longjmp'd to
extern "C" void nx_longjmp(NxJmp* buf, int val);

}
