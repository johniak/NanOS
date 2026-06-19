/*
 * Interrupt64.cpp — dispatch from the asm stubs to the C handler table.
 *
 * isr_handler / irq_handler take a Registers* (a pointer to the on-stack frame), NOT a copy:
 * in 64-bit there is no popa, so the stub restores registers from the very frame the handler
 * may have edited. irq_handler also acknowledges the PIC (EOI) before dispatching.
 */
#include "Interrupt64.h"
#include "Port64.h"
#include "SignalDispatch.h"   // kernel::signalDeliver()

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
    // On the way back to ring 3, deliver pending signals (mirror i686 Interrupt.cpp). A fault
    // handler that kills the process does not return here, so this only runs for recoverable
    // ring-3 exceptions. Not a syscall return -> no restart.
    if ((regs->cs & 3) == 3)
        kernel::signalDeliver((arch::TrapFrame*) regs, 0, false);
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
    // On the way back to ring 3, deliver pending signals (e.g. a SIGINT posted by the keyboard
    // IRQ, or SIGKILL/SIGALRM, to a CPU-bound foreground process that makes no syscalls).
    // Mirror i686 Interrupt.cpp; the deferred-preemption schedPreempt call in irq64.S runs after
    // this. Not a syscall return -> no restart.
    if ((regs->cs & 3) == 3)
        kernel::signalDeliver((arch::TrapFrame*) regs, 0, false);
}
