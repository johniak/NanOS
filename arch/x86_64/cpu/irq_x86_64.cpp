/*
 * irq_x86_64.cpp — x86-64 implementation of <arch/irq.h>.
 *
 * Maps arch-neutral IRQ lines to the remapped PIC vectors (IRQ_TIMER -> 32, IRQ_KEYBOARD ->
 * 33) and wraps the kernel::Interrupt handler table. The x86-64 TrapFrame is kernel::Registers;
 * the function-pointer cast is the MI/MD boundary (identical pattern to i686 irq_x86.cpp).
 */
#include <arch/irq.h>
#include "Interrupt64.h"

namespace arch {

void registerIrqHandler(unsigned irq, IrqHandler h) {
    kernel::Interrupt::registerInterruptHandler((unsigned char) (IRQ0 + irq), (kernel::IsrHandler) h);
}

void registerTrapHandler(unsigned vector, IrqHandler h) {
    kernel::Interrupt::registerInterruptHandler((unsigned char) vector, (kernel::IsrHandler) h);
}

}  // namespace arch
