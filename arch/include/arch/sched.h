/*
 * arch/sched.h — MI/MD contract for the scheduler's machine-dependent bits.
 *
 * The MI Scheduler owns the task table, states and round-robin policy; the arch
 * provides the actual CPU context switch (stack-pointer swap + CR3), the fabricated
 * first-run stack, and the preemption timer.
 */
#pragma once

namespace arch {

// Save the current context (callee-saved regs + esp + CR3) into *saveOldKesp, then
// load newKesp's context. Returns (on this task) when something switches back to it.
extern "C" void archContextSwitch(unsigned* saveOldKesp, unsigned newKesp);

// Fabricate a fresh task's kernel stack so the first archContextSwitch into the
// returned kesp "returns" into the task trampoline (which runs the task body).
unsigned archTaskBootstrap(unsigned char* kstackTop, unsigned cr3);

// Physical address of the kernel page directory (initial CR3 for a new task).
unsigned archKernelCr3();

// Repoint the kernel stack the CPU lands on for the next ring3->ring0 trap (TSS.esp0).
// The scheduler calls this on every switch so traps land on the running task's stack.
void setKernelStack(unsigned esp0);

// Program the preemption timer at `hz` and route its IRQ to the scheduler tick.
void archTimerInit(unsigned hz);

// Idle primitive: enable interrupts and halt until the next one.
void halt_or_hlt();

}
