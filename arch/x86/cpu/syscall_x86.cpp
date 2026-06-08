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
#include "SignalDispatch.h"    // kernel::signalDeliver()
#include "Exec.h"              // kernel::procExit()

namespace {

void syscallTrap(kernel::Registers* r) {
	unsigned origEax = r->eax;   // syscall number, saved before dispatch (for restart)
	r->eax = (unsigned) kernel::kernelSyscall(r->eax, r->ebx, r->ecx, r->edx,
			(arch::TrapFrame*) r);
	// If the process exited (SYS_exit set the flag), tear it down and schedule away.
	// procExit does not return.
	if (kernel::kernelSyscalls()->hasExited())
		kernel::procExit();
	// Deliver pending signals on the way back to ring 3 (may terminate, run a handler, or
	// restart this syscall if it was interrupted).
	if ((r->cs & 3) == 3)
		kernel::signalDeliver((arch::TrapFrame*) r, origEax, true);
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
	// Exercise the int 0x80 round-trip at boot (catches a broken IDT gate / handler)
	// without printing — a zero-length write still traps and returns through the gate.
	const char* msg = "";
	sys3(SYS_write, 1, (int) msg, 0);
}

}  // namespace arch
