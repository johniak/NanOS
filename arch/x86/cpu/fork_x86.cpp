/*
 * fork_x86.cpp — x86 child-stack fabrication for fork (<arch/sched.h>).
 *
 * The child of fork() must, on its first scheduling, resume as if it were returning
 * from the same int 0x80 the parent issued — but with eax = 0. We build its kernel
 * stack so that:
 *   1. archContextSwitch (switch.S) pops cr3/ebp/edi/esi/ebx and `ret`s, and
 *   2. that `ret` lands in ret_from_fork (isr.S), whose tail restores the segment
 *      registers, `popad`s, drops the int_no/err_code, and `iret`s a copied trap
 *      frame back into ring 3.
 *
 * Stack layout we plant (low -> high address; kesp points at the lowest word):
 *   [cr3][ebp=0][edi=0][esi=0][ebx=0][ret=ret_from_fork]   <- context-switch frame
 *   [ Registers copy of the parent's trap frame, eax = 0 ] <- iret'd by ret_from_fork
 */
#include <arch/sched.h>
#include "Interrupt.h"   // kernel::Registers (the x86 TrapFrame)
#include "Scheduler.h"   // kernel::Task

extern "C" void ret_from_fork();   // isr.S

namespace arch {

void archForkChild(kernel::Task* child, TrapFrame* parentTf, uint64_t childCr3) {
	kernel::Registers* parent = (kernel::Registers*) parentTf;

	// Copy the parent's full trap frame to the top of the child's kernel stack.
	unsigned char* top = (unsigned char*) child->esp0;
	kernel::Registers* frame = ((kernel::Registers*) top) - 1;
	*frame = *parent;
	frame->eax = 0;   // fork() returns 0 in the child

	// Below the frame, the context-switch save area archContextSwitch will pop.
	unsigned* sp = (unsigned*) frame;
	*--sp = (unsigned) ret_from_fork;   // ret target after the 5 pops
	*--sp = 0;                          // ebp
	*--sp = 0;                          // edi
	*--sp = 0;                          // esi
	*--sp = 0;                          // ebx
	*--sp = (uint32_t) childCr3;        // cr3 (popped first, loaded into CR3) — i686 CR3 is 32-bit
	child->kesp = (uintptr_t) sp;
}

// clone (thread create): same fabrication as archForkChild, but the new thread runs on its
// own user stack — so after copying the parent's trap frame we override `useresp` (the ring-3
// ESP iret restores) with the caller-supplied childUserEsp. `cr3` is the SHARED directory phys
// (the thread keeps the parent's address space); the context-switch frame still reloads it so
// archContextSwitch's unconditional CR3 write lands on a valid (here, unchanged) directory.
void archCloneChild(kernel::Task* child, TrapFrame* parentTf, uint64_t cr3, uintptr_t childUserEsp) {
	kernel::Registers* parent = (kernel::Registers*) parentTf;

	unsigned char* top = (unsigned char*) child->esp0;
	kernel::Registers* frame = ((kernel::Registers*) top) - 1;
	*frame = *parent;
	frame->eax = 0;                     // clone() returns 0 in the new thread
	frame->useresp = (uint32_t) childUserEsp;   // ... which runs on its own stack (i686 ESP is 32-bit)

	unsigned* sp = (unsigned*) frame;
	*--sp = (unsigned) ret_from_fork;   // ret target after the 5 pops
	*--sp = 0;                          // ebp
	*--sp = 0;                          // edi
	*--sp = 0;                          // esi
	*--sp = 0;                          // ebx
	*--sp = (uint32_t) cr3;             // cr3 (popped first, loaded into CR3) — shared dir, 32-bit on i686
	child->kesp = (uintptr_t) sp;
}

}  // namespace arch
