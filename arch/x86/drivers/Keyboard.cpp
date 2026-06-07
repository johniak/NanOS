/*
 * Keyboard.cpp — PS/2 keyboard (IRQ1). Thin: read the scancode and forward it to
 * the arch input layer (input_x86.cpp), which owns the cooked/raw policy.
 */
#include "Keyboard.h"
#include "Idt.h"

namespace arch { void inputFeedScancode(unsigned char sc); }

namespace kernel {

static void kb_handler(Registers* reg) {
	(void) reg;
	arch::inputFeedScancode((unsigned char) IOPort::inb(0x60));
}

void Keyboard::initialize() {
	Interrupt::registerInterruptHandler(IRQ1, kb_handler);
}

}
