/*
 * Interrupt.cpp
 *
 *  Created on: Feb 3, 2014
 *      Author: johniak
 */
#include "Interrupt.h"
#include "Console.h"
#include "IOPort.h"
#include "SignalDispatch.h"   // kernel::signalDeliver()

namespace kernel {
static IsrHandler interruptHandlers[256];

void Interrupt::registerInterruptHandler(unsigned char n, IsrHandler handler) {
	interruptHandlers[n] = handler;
}

}

//called from isr.S asm
extern "C" void isr_handler(kernel::Registers regs) {
	if (kernel::interruptHandlers[regs.int_no] != 0) {
		kernel::IsrHandler handler = kernel::interruptHandlers[regs.int_no];
		handler(&regs);
	}
}

//called from irq.S asm
extern "C" void irq_handler(kernel::Registers regs) {
	// Send an EOI (end of interrupt) signal to the PICs.
	// If this interrupt involved the slave.
	if (regs.int_no >= 40) {
		// Send reset signal to slave.
		kernel::IOPort::outb(0xA0, 0x20);
	}
	// Send reset signal to master. (As well as slave, if necessary).
	kernel::IOPort::outb(0x20, 0x20);

	if (kernel::interruptHandlers[regs.int_no] != 0) {
		kernel::IsrHandler handler = kernel::interruptHandlers[regs.int_no];
		handler(&regs);
	}
	// On the way back to ring 3, deliver pending signals (e.g. a SIGINT posted by the
	// keyboard IRQ to a CPU-bound foreground process). Skipped for kernel-mode frames.
	if ((regs.cs & 3) == 3)
		kernel::signalDeliver((arch::TrapFrame*) &regs);
}

