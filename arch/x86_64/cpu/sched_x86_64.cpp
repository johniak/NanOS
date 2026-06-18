/*
 * sched_x86_64.cpp — x86_64 implementation of <arch/sched.h>.
 *
 * The context switch itself is in switch64.S; here we fabricate a new task's first-run
 * kernel stack, expose the kernel CR3, drive the idle primitive, and wire the PIT to the
 * scheduler tick. schedulerRunCurrentBody bridges the asm trampoline to the MI scheduler.
 * Translated from arch/x86/cpu/sched_x86.cpp; the LP64 differences are 64-bit stack slots
 * (CR3 + the SysV callee-saved set) and setKernelStack updating both the TSS rsp0 and the
 * Plan-6 per-CPU syscall kernel-stack pointer (decision #B).
 */
#include <arch/sched.h>
#include <arch/mmu.h>           // arch::mmuKernelDirPhys
#include "Interrupt64.h"        // kernel::Registers, kernel::Interrupt, IRQ0
#include "Port64.h"             // kernel::port_outb
#include "Scheduler.h"

extern "C" void taskTrampoline();
extern "C" void schedulerRunCurrentBody() { kernel::Scheduler::runCurrentBody(); }

// Called from irq_common_stub (irq64.S) on the way back to ring 3: perform a deferred
// reschedule if the timer asked for one. Switching here -- with a full, clean trap frame on
// the kernel stack -- is the only place a user task is involuntarily preempted.
extern "C" void schedPreempt() { kernel::Scheduler::preempt(); }

namespace arch {

// Defined in the sibling translation units; both must point at the running task's kernel stack
// so a ring3->ring0 entry (interrupt via TSS.rsp0, or SYSCALL via the per-CPU block) lands on it.
void cpuSetTssKernelStack(uint64_t rsp0);   // cpu_x86_64.cpp (Gdt64.setKernelStack -> TSS.rsp0)
void syscallSetKernelStack(uint64_t top);   // syscall_x86_64.cpp (g_percpu.kernelStackTop)

uint64_t archKernelCr3() { return mmuKernelDirPhys(); }   // kernel page-dir phys (initial CR3)

void halt_or_hlt() { __asm__ __volatile__("sti; hlt"); }

uintptr_t archTaskBootstrap(unsigned char* kstackTop, uint64_t cr3) {
	// Fabricate the stack so the first archContextSwitch into it pops (cr3, r15, r14, r13,
	// r12, rbp, rbx) and `ret`s into taskTrampoline — exactly the layout archContextSwitch
	// saves. Plant high address first (decrementing sp): the return address sits highest, cr3
	// lowest (popped first). 8 slots * 8 bytes = 64; a 16-aligned kstackTop stays 16-aligned.
	uint64_t* sp = (uint64_t*) kstackTop;
	*--sp = (uint64_t) taskTrampoline;   // ret target after the 7 pops
	*--sp = 0;                           // rbx
	*--sp = 0;                           // rbp
	*--sp = 0;                           // r12
	*--sp = 0;                           // r13
	*--sp = 0;                           // r14
	*--sp = 0;                           // r15
	*--sp = cr3;                         // cr3 (popped first, reloaded into CR3)
	return (uintptr_t) sp;
}

// Repoint BOTH kernel-stack pointers the CPU lands on for the next ring3->ring0 entry: the TSS
// rsp0 (used by interrupt/exception gates) and the per-CPU syscall block (used by the SYSCALL
// fast path). The scheduler calls this on every switch so traps land on the running task's stack.
void setKernelStack(uintptr_t rsp0) {
	cpuSetTssKernelStack((uint64_t) rsp0);
	syscallSetKernelStack((uint64_t) rsp0);
}

// The timer tick. The interrupted frame's CS tells us whether we preempted ring 3 (user) or
// ring 0 (kernel), so the scheduler can split CPU time into user vs system.
static void timerTick(kernel::Registers* r) {
	kernel::Scheduler::onTick((r->cs & 3) == 3);
}

void archTimerInit(unsigned hz) {
	unsigned divisor = 1193180u / hz;            // 1000 Hz -> 1193
	kernel::port_outb(0x43, 0x36);               // channel 0, lobyte/hibyte, mode 3 (square wave)
	kernel::port_outb(0x40, (unsigned char) (divisor & 0xFF));
	kernel::port_outb(0x40, (unsigned char) ((divisor >> 8) & 0xFF));
	// IRQ0 (remapped vector 32) -> irq64.S -> irq_handler (sends EOI) -> this handler.
	kernel::Interrupt::registerInterruptHandler(IRQ0, &timerTick);
}

}  // namespace arch
