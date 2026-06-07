/*
 * usermode_x86.cpp — x86 implementation of <arch/usermode.h>.
 *
 * Runs a loaded program. execUserImage builds a per-process address space, maps
 * the image + a stack to fresh private USER frames (copying the staged bytes),
 * and enters ring 3 via iret. The program traps to the kernel via int 0x80 (the
 * CPU switches to TSS.esp0); on exit the syscall trap calls userExit(), which
 * switches CR3 back to the kernel directory and longjmps back into enterUser.
 */
#include <arch/usermode.h>
#include <arch/mmu.h>
#include "NxJmp.h"
#include "PagingControl.h"     // kernel::loadCr3
#include "FrameAllocator.h"    // kernel::g_frames
#include "SyscallDispatch.h"   // kernel::kernelSyscalls()
#include <string.h>

namespace {
arch::NxJmp g_userCtx;

const uint32_t USER_STACK_TOP = 0x500000;
const uint32_t USER_STACK_BOT = 0x4F0000;   // 64 KiB user stack
}

namespace arch {

int enterUser(uint32_t entry, uint32_t userStackTop, AddressSpace* space) {
	if (nx_setjmp(&g_userCtx) != 0)
		return kernel::kernelSyscalls()->code();   // returned here via userExit()

	if (space == 0) {
		((void (*)()) entry)();   // ring-0 path
		return 0;
	}

	// Ring 3: switch to the process address space, then iret down to CPL 3.
	__asm__ __volatile__("cli");
	mmuSwitch(space);
	__asm__ __volatile__(
			"mov $0x23, %%ax\n\t"   // user data selector (DPL 3)
			"mov %%ax, %%ds\n\t"
			"mov %%ax, %%es\n\t"
			"mov %%ax, %%fs\n\t"
			"mov %%ax, %%gs\n\t"
			"pushl $0x23\n\t"        // ss  (user data)
			"pushl %0\n\t"           // esp (user stack top)
			"pushl $0x202\n\t"       // eflags (IF=1)
			"pushl $0x1B\n\t"        // cs  (user code, DPL 3)
			"pushl %1\n\t"           // eip (entry)
			"iret\n\t"
			:
			: "r"(userStackTop), "r"(entry)
			: "ax", "memory");
	return 0;   // not reached
}

void userExit() {
	// Running at CPL 0 on the esp0 stack with the process directory active.
	// Switch back to the kernel directory, then longjmp to the saved kernel
	// context (its stack + code are in the shared kernel half, mapped in both).
	kernel::loadCr3(mmuKernelDirPhys());
	nx_longjmp(&g_userCtx, 1);   // val=1; real exit code comes from Syscalls::code()
}

int execUserImage(uint32_t entry, uint32_t loadBase, uint32_t bssEnd) {
	AddressSpace* space = mmuCreateAddressSpace();

	// Map the image (code/data/bss) to fresh private USER frames, copying the
	// staged bytes (identity-mapped in the kernel dir) into each frame. NxeLoader
	// already zeroed the staged bss, so copying those pages yields zeros.
	uint32_t imgEnd = (bssEnd + 0xFFF) & ~0xFFFu;
	for (uint32_t va = loadBase; va < imgEnd; va += 0x1000) {
		uint32_t f = kernel::g_frames.alloc();
		memcpy((void*) f, (void*) va, 0x1000);
		mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	}

	// Map a private, zeroed user stack at the top of the window (disjoint from the
	// tiny init image below it).
	for (uint32_t va = USER_STACK_BOT; va < USER_STACK_TOP; va += 0x1000) {
		uint32_t f = kernel::g_frames.alloc();
		memset((void*) f, 0, 0x1000);
		mmuMap(space, va, f, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
	}

	int rc = enterUser(entry, USER_STACK_TOP, space);
	mmuDestroyAddressSpace(space);
	return rc;
}

}  // namespace arch
