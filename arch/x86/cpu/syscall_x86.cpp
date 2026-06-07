/*
 * syscall_x86.cpp — x86 syscall trap: implements <arch/syscall.h>.
 *
 * int 0x80, Linux i386 ABI: nr in eax, args in ebx/ecx/edx, return in eax
 * (propagates to the caller via isr_common_stub's popa). The number->Syscalls
 * mapping is machine-independent (kernel::kernelSyscall).
 */
#include <arch/syscall.h>
#include <arch/usermode.h>
#include "Interrupt.h"
#include "Syscall.h"
#include "SyscallDispatch.h"   // kernel::kernelSyscalls()
#include "Exec.h"              // kernel::procExit()

namespace {

void syscallTrap(kernel::Registers* r) {
	r->eax = (unsigned) kernel::kernelSyscall(r->eax, r->ebx, r->ecx, r->edx,
			(arch::TrapFrame*) r);
	// If the process exited (SYS_exit set the flag), tear it down and schedule away.
	// procExit does not return.
	if (kernel::kernelSyscalls()->hasExited())
		kernel::procExit();
}

// Issue a Linux-style syscall via int 0x80 (nr in eax, args in ebx/ecx/edx).
int sys3(int nr, int a, int b, int c) {
	int ret;
	asm volatile("int $0x80" : "=a"(ret) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return ret;
}

}  // namespace

namespace arch {

void syscallInit() {
	kernel::Interrupt::registerInterruptHandler(0x80, &syscallTrap);
}

void syscallSelfTest() {
	const char* msg = "syscall write OK\n";
	sys3(SYS_write, 1, (int) msg, 17);
}

}  // namespace arch
