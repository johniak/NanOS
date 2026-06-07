/*
 * irq_x86.cpp — x86 implementation of <arch/irq.h>.
 *
 * Maps arch-neutral IRQ lines to the remapped PIC vectors (IRQ_TIMER -> 32,
 * IRQ_KEYBOARD -> 33) and wraps the existing Interrupt handler table. The x86
 * TrapFrame is kernel::Registers; the function-pointer bridge is the arch
 * boundary cast.
 */
#include <arch/irq.h>
#include "Interrupt.h"

namespace arch {

void registerIrqHandler(unsigned irq, IrqHandler h) {
	kernel::Interrupt::registerInterruptHandler(IRQ0 + irq, (kernel::IsrHandler) h);
}

void registerTrapHandler(unsigned vector, IrqHandler h) {
	kernel::Interrupt::registerInterruptHandler(vector, (kernel::IsrHandler) h);
}

}
