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

void archForkChild(kernel::Task* child, TrapFrame* parentTf, unsigned childCr3) {
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
	*--sp = childCr3;                   // cr3 (popped first, loaded into CR3)
	child->kesp = (unsigned) sp;
}

}  // namespace arch
