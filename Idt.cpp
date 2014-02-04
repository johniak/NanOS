#include "Idt.h"


namespace kernel {

void Idt::initialize() {
	idtPtr.limit = sizeof(IdtEntry) * 256 - 1;
	idtPtr.base = (unsigned) &idtEntries;
	memset(&idtEntries, 0, sizeof(IdtEntry) * 256);

	IOPort::outb(0x20, 0x11);
	IOPort::outb(0xA0, 0x11);
	IOPort::outb(0x21, 0x20);
	IOPort::outb(0xA1, 0x28);
	IOPort::outb(0x21, 0x04);
	IOPort::outb(0xA1, 0x02);
	IOPort::outb(0x21, 0x01);
	IOPort::outb(0xA1, 0x01);
	IOPort::outb(0x21, 0x0);
	IOPort::outb(0xA1, 0x0);

	setGate(0, (unsigned) isr0, 0x08, 0x8E);
	setGate(1, (unsigned) isr1, 0x08, 0x8E);
	setGate(2, (unsigned) isr2, 0x08, 0x8E);
	setGate(3, (unsigned) isr3, 0x08, 0x8E);
	setGate(4, (unsigned) isr4, 0x08, 0x8E);
	setGate(5, (unsigned) isr5, 0x08, 0x8E);
	setGate(6, (unsigned) isr6, 0x08, 0x8E);
	setGate(7, (unsigned) isr7, 0x08, 0x8E);
	setGate(8, (unsigned) isr8, 0x08, 0x8E);
	setGate(9, (unsigned) isr9, 0x08, 0x8E);
	setGate(10, (unsigned) isr10, 0x08, 0x8E);
	setGate(11, (unsigned) isr11, 0x08, 0x8E);
	setGate(12, (unsigned) isr12, 0x08, 0x8E);
	setGate(13, (unsigned) isr13, 0x08, 0x8E);
	setGate(14, (unsigned) isr14, 0x08, 0x8E);
	setGate(15, (unsigned) isr15, 0x08, 0x8E);
	setGate(16, (unsigned) isr16, 0x08, 0x8E);
	setGate(17, (unsigned) isr17, 0x08, 0x8E);
	setGate(18, (unsigned) isr18, 0x08, 0x8E);
	setGate(19, (unsigned) isr19, 0x08, 0x8E);
	setGate(20, (unsigned) isr20, 0x08, 0x8E);
	setGate(21, (unsigned) isr21, 0x08, 0x8E);
	setGate(22, (unsigned) isr22, 0x08, 0x8E);
	setGate(23, (unsigned) isr23, 0x08, 0x8E);
	setGate(24, (unsigned) isr24, 0x08, 0x8E);
	setGate(25, (unsigned) isr25, 0x08, 0x8E);
	setGate(26, (unsigned) isr26, 0x08, 0x8E);
	setGate(27, (unsigned) isr27, 0x08, 0x8E);
	setGate(28, (unsigned) isr28, 0x08, 0x8E);
	setGate(29, (unsigned) isr29, 0x08, 0x8E);
	setGate(30, (unsigned) isr30, 0x08, 0x8E);
	setGate(31, (unsigned) isr31, 0x08, 0x8E);
	setGate(32, (unsigned) irq0, 0x08, 0x8E);
	setGate(33, (unsigned) irq1, 0x08, 0x8E);
	setGate(34, (unsigned) irq2, 0x08, 0x8E);
	setGate(35, (unsigned) irq3, 0x08, 0x8E);
	setGate(36, (unsigned) irq4, 0x08, 0x8E);
	setGate(37, (unsigned) irq5, 0x08, 0x8E);
	setGate(38, (unsigned) irq6, 0x08, 0x8E);
	setGate(39, (unsigned) irq7, 0x08, 0x8E);
	setGate(40, (unsigned) irq8, 0x08, 0x8E);
	setGate(41, (unsigned) irq9, 0x08, 0x8E);
	setGate(42, (unsigned) irq10, 0x08, 0x8E);
	setGate(43, (unsigned) irq11, 0x08, 0x8E);
	setGate(44, (unsigned) irq12, 0x08, 0x8E);
	setGate(45, (unsigned) irq13, 0x08, 0x8E);
	setGate(46, (unsigned) irq14, 0x08, 0x8E);
	setGate(47, (unsigned) irq15, 0x08, 0x8E);

	idt_load((unsigned) &idtPtr);
}

void Idt::setGate(unsigned char num, unsigned base, unsigned short sel,
		unsigned char flags) {
	idtEntries[num].base_lo = base & 0xFFFF;
	idtEntries[num].base_hi = (base >> 16) & 0xFFFF;
	idtEntries[num].sel = sel;
	idtEntries[num].always0 = 0;
	idtEntries[num].flags = flags /* | 0x60 */;
}

}
