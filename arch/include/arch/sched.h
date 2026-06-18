/*
 * arch/sched.h — MI/MD contract for the scheduler's machine-dependent bits.
 *
 * The MI Scheduler owns the task table, states and round-robin policy; the arch
 * provides the actual CPU context switch (stack-pointer swap + CR3), the fabricated
 * first-run stack, and the preemption timer.
 */
#pragma once

#include <stdint.h>

namespace kernel { struct Task; }   // scheduler task (Scheduler.h)

namespace arch {

struct TrapFrame;   // opaque syscall trap frame (the x86 Registers)

// Address-typed parameters use uintptr_t (kernel stack pointers) / uint64_t (CR3 phys) so the
// SAME contract serves i686 (32-bit) and x86_64 (64-bit): uintptr_t == uint32_t on i686-elf, so
// the i686 impl behaviour is unchanged, while the x86_64 impl gets 64-bit stack pointers + CR3.

// Save the current context (callee-saved regs + esp + CR3) into *saveOldKesp, then
// load newKesp's context. Returns (on this task) when something switches back to it.
extern "C" void archContextSwitch(uintptr_t* saveOldKesp, uintptr_t newKesp);

// Fabricate a fresh task's kernel stack so the first archContextSwitch into the
// returned kesp "returns" into the task trampoline (which runs the task body).
uintptr_t archTaskBootstrap(unsigned char* kstackTop, uint64_t cr3);

// Physical address of the kernel page directory (initial CR3 for a new task).
uint64_t archKernelCr3();

// Repoint the kernel stack the CPU lands on for the next ring3->ring0 trap (TSS.esp0).
// The scheduler calls this on every switch so traps land on the running task's stack.
void setKernelStack(uintptr_t esp0);

// Program the preemption timer at `hz` and route its IRQ to the scheduler tick.
void archTimerInit(unsigned hz);

// Idle primitive: enable interrupts and halt until the next one.
void halt_or_hlt();

// fork: fabricate the child task's kernel stack from the parent's syscall trap frame
// so the first context switch into it `ret`s through ret_from_fork and `iret`s the
// copied frame (eax = 0) into ring 3 under `childCr3`. `child` already has kstack/esp0.
void archForkChild(kernel::Task* child, TrapFrame* parentTf, uint64_t childCr3);

// clone (thread create): identical to archForkChild — copy the parent's trap frame, set
// eax=0, build the ret_from_fork context-switch frame — EXCEPT the new thread runs on its
// OWN user stack, so the copied frame's ring-3 ESP is set to `childUserEsp`. `cr3` is the
// SHARED (parent's) page-directory phys (a thread does not get a private address space).
void archCloneChild(kernel::Task* child, TrapFrame* parentTf, uint64_t cr3, uintptr_t childUserEsp);

}
