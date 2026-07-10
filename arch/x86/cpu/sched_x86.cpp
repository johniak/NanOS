/*
 * sched_x86.cpp — x86 implementation of <arch/sched.h>.
 *
 * The context switch itself is in switch.S; here we fabricate a new task's first-run
 * kernel stack, expose the kernel CR3, drive the idle primitive, and wire the PIT to
 * the scheduler tick. schedulerRunCurrentBody bridges the asm trampoline to the MI
 * scheduler.
 */
#include <arch/sched.h>
#include "Interrupt.h"
#include "IOPort.h"
#include "PagingControl.h"   // kernel::readCr3
#include "Scheduler.h"

extern "C" void taskTrampoline();
extern "C" void schedulerRunCurrentBody() { kernel::Scheduler::runCurrentBody(); }

// Called from irq_common_stub (irq.S) on the way back to ring 3: perform a deferred
// reschedule if the timer asked for one. Switching here -- with a full, clean trap frame
// on the kernel stack -- is the only place a user task is involuntarily preempted.
extern "C" void schedPreempt() { kernel::Scheduler::preempt(); }

namespace arch {

// FPU/SSE context helpers are x86_64-only (the frozen i686 userland predates SSE codegen and its
// switch.S ignores the extra archContextSwitch args). No-ops so the shared MI code links.
void archFpuCapture(void*) {}
void archFpuLoad(const void*) {}


uint64_t archKernelCr3() { return kernel::readCr3(); }   // kernel dir at create time

void halt_or_hlt() { __asm__ __volatile__("sti; hlt"); }

uintptr_t archTaskBootstrap(unsigned char* kstackTop, uint64_t cr3) {
	// Fabricate the stack so the first archContextSwitch pops (cr3, ebp, edi, esi,
	// ebx) and `ret`s into taskTrampoline — exactly the layout archContextSwitch saves.
	unsigned* sp = (unsigned*) kstackTop;
	*--sp = (unsigned) taskTrampoline;   // ret target
	*--sp = 0;                           // ebp
	*--sp = 0;                           // edi
	*--sp = 0;                           // esi
	*--sp = 0;                           // ebx
	*--sp = (uint32_t) cr3;              // cr3 (popped first) — i686 CR3 is 32-bit
	return (uintptr_t) sp;
}

// The timer tick. The interrupted frame's CS tells us whether we preempted ring 3 (user) or
// ring 0 (kernel), so the scheduler can split CPU time into user vs system.
static void timerTick(kernel::Registers* r) {
	kernel::Scheduler::onTick((r->cs & 3) == 3);
}

void archTimerInit(unsigned hz) {
	unsigned divisor = 1193180u / hz;    // 1000 Hz -> 1193
	kernel::IOPort::outb(0x43, 0x36);    // channel 0, lobyte/hibyte, mode 3
	kernel::IOPort::outb(0x40, (unsigned char) (divisor & 0xFF));
	kernel::IOPort::outb(0x40, (unsigned char) ((divisor >> 8) & 0xFF));
	kernel::Interrupt::registerInterruptHandler(IRQ0, &timerTick);
}

}  // namespace arch
