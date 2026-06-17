/*
 * Interrupt64.cpp — dispatch from the asm stubs to the C handler table.
 *
 * isr_handler / irq_handler take a Registers* (a pointer to the on-stack frame), NOT a copy:
 * in 64-bit there is no popa, so the stub restores registers from the very frame the handler
 * may have edited. irq_handler also acknowledges the PIC (EOI) before dispatching.
 */
#include "Interrupt64.h"
#include "Port64.h"

namespace kernel {
static IsrHandler interruptHandlers[256];

void Interrupt::registerInterruptHandler(unsigned char n, IsrHandler handler) {
    interruptHandlers[n] = handler;
}
}  // namespace kernel

// Called from isr64.S (CPU exceptions 0..31).
extern "C" void isr_handler(kernel::Registers* regs) {
    if (kernel::interruptHandlers[regs->int_no] != 0)
        kernel::interruptHandlers[regs->int_no](regs);
}

// Called from irq64.S (device IRQs, vectors 32..47).
extern "C" void irq_handler(kernel::Registers* regs) {
    // EOI: acknowledge the slave PIC first if the IRQ came from it (vector >= 40), then the
    // master — otherwise the PIC won't deliver further interrupts on that line.
    if (regs->int_no >= 40)
        kernel::port_outb(0xA0, 0x20);
    kernel::port_outb(0x20, 0x20);

    if (kernel::interruptHandlers[regs->int_no] != 0)
        kernel::interruptHandlers[regs->int_no](regs);
}
