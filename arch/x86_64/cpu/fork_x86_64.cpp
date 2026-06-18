/*
 * fork_x86_64.cpp — x86-64 child-stack fabrication for fork/clone (<arch/sched.h>).
 *
 * The child of fork() must, on its first scheduling, resume as if it were returning
 * from the same syscall the parent issued — but with rax = 0. We build its kernel
 * stack so that:
 *   1. archContextSwitch (switch64.S) pops cr3/r15/r14/r13/r12/rbp/rbx and `ret`s, and
 *   2. that `ret` lands in ret_from_fork (switch64.S), whose tail restores the 15 GPRs,
 *      `swapgs` if returning to ring 3, drops int_no/err_code, and `iretq`s a copied
 *      trap frame back into ring 3.
 *
 * Stack layout we plant (low -> high address; kesp points at the lowest word):
 *   [cr3][r15=0][r14=0][r13=0][r12=0][rbp=0][rbx=0][ret=ret_from_fork] <- context-switch frame
 *   [ kernel::Registers copy of the parent's trap frame, rax = 0 ]     <- iretq'd by ret_from_fork
 *
 * The context-switch frame's field order mirrors archContextSwitch's pop order exactly
 * (cr3 popped first at the lowest address, then r15..r12, rbp, rbx, then `ret`).
 */
#include <arch/sched.h>
#include "Interrupt64.h"   // kernel::Registers (the x86-64 TrapFrame)
#include "Scheduler.h"     // kernel::Task

extern "C" void ret_from_fork();   // switch64.S

namespace arch {

void archForkChild(kernel::Task* child, TrapFrame* parentTf, uint64_t childCr3) {
	kernel::Registers* parent = (kernel::Registers*) parentTf;

	// Copy the parent's full trap frame to the top of the child's kernel stack.
	unsigned char* top = (unsigned char*) child->esp0;
	kernel::Registers* frame = ((kernel::Registers*) top) - 1;
	*frame = *parent;
	frame->rax = 0;   // fork() returns 0 in the child

	// Below the frame, the context-switch save area archContextSwitch will pop.
	uint64_t* sp = (uint64_t*) frame;
	*--sp = (uint64_t) ret_from_fork;   // ret target after the 7 pops
	*--sp = 0;                          // rbx
	*--sp = 0;                          // rbp
	*--sp = 0;                          // r12
	*--sp = 0;                          // r13
	*--sp = 0;                          // r14
	*--sp = 0;                          // r15
	*--sp = childCr3;                   // cr3 (popped first, loaded into CR3)
	child->kesp = (uintptr_t) sp;
}

// clone (thread create): same fabrication as archForkChild, but the new thread runs on its
// own user stack — so after copying the parent's trap frame we override `rsp` (the ring-3
// RSP iretq restores) with the caller-supplied childUserEsp. `cr3` is the SHARED directory
// phys (the thread keeps the parent's address space); the context-switch frame still reloads
// it so archContextSwitch's CR3 compare/write lands on a valid (here, unchanged) PML4.
void archCloneChild(kernel::Task* child, TrapFrame* parentTf, uint64_t cr3, uintptr_t childUserEsp) {
	kernel::Registers* parent = (kernel::Registers*) parentTf;

	unsigned char* top = (unsigned char*) child->esp0;
	kernel::Registers* frame = ((kernel::Registers*) top) - 1;
	*frame = *parent;
	frame->rax = 0;                     // clone() returns 0 in the new thread
	frame->rsp = (uint64_t) childUserEsp;   // ... which runs on its own stack

	uint64_t* sp = (uint64_t*) frame;
	*--sp = (uint64_t) ret_from_fork;   // ret target after the 7 pops
	*--sp = 0;                          // rbx
	*--sp = 0;                          // rbp
	*--sp = 0;                          // r12
	*--sp = 0;                          // r13
	*--sp = 0;                          // r14
	*--sp = 0;                          // r15
	*--sp = cr3;                        // cr3 (popped first, loaded into CR3) — shared dir
	child->kesp = (uintptr_t) sp;
}

}  // namespace arch
