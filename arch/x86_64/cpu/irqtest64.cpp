/*
 * irqtest64.cpp — Plan-4 bring-up self-test for the x86-64 interrupt path.
 *
 * Installs two device-IRQ handlers via the MI <arch/irq.h> registration (proving that path
 * works unchanged): IRQ0 (PIT @ ~100 Hz) prints one '.' per second as a heartbeat, and IRQ1
 * (PS/2 keyboard) echoes the make-code of each key press. This is the interrupt analogue of
 * Plan 2's banner; the real keyboard driver is a loadable kext (later).
 */
#include "Interrupt64.h"
#include "Port64.h"
#include "Console.h"
#include <arch/irq.h>

namespace {
volatile unsigned long g_ticks = 0;

// IRQ0: programmable interval timer. One dot per 100 ticks (~1 s) = visible proof it fires.
void pitTick(kernel::Registers*) {
    if (++g_ticks % 100 == 0)
        kernel::Console::write(".");
}

// IRQ1: read the scancode from the PS/2 data port. Echo only make-codes (bit 7 clear); break
// codes (key release) are ignored for a clean display.
void kbdEcho(kernel::Registers*) {
    unsigned char sc = kernel::port_inb(0x60);
    if (!(sc & 0x80)) {
        kernel::Console::write("[key sc=");
        kernel::Console::writeHex((int) sc);
        kernel::Console::write("]");
    }
}
}  // namespace

namespace kernel {

void irqSelfTest() {
    // PIT channel 0, mode 3 (square wave), ~100 Hz: divisor = 1193180 / 100.
    unsigned divisor = 1193180u / 100u;
    port_outb(0x43, 0x36);
    port_outb(0x40, (unsigned char) (divisor & 0xFF));
    port_outb(0x40, (unsigned char) ((divisor >> 8) & 0xFF));

    arch::registerIrqHandler(arch::IRQ_TIMER,    (arch::IrqHandler) pitTick);
    arch::registerIrqHandler(arch::IRQ_KEYBOARD, (arch::IrqHandler) kbdEcho);
}

}  // namespace kernel
