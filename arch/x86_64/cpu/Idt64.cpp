#include "Idt64.h"
#include "Port64.h"
#include <string.h>

namespace kernel {

void Idt64::setGate(unsigned char num, uint64_t handler, uint16_t sel, uint8_t ist, uint8_t flags) {
    IdtEntry64& e = entries[num];
    e.base_lo  = (uint16_t) (handler & 0xFFFF);
    e.base_mid = (uint16_t) ((handler >> 16) & 0xFFFF);
    e.base_hi  = (uint32_t) ((handler >> 32) & 0xFFFFFFFF);
    e.sel      = sel;
    e.ist      = (uint8_t) (ist & 0x07);
    e.flags    = flags;
    e.reserved = 0;
}

void Idt64::initialize() {
    memset(&entries, 0, sizeof(entries));

    // Remap the PIC to 0x20 (master) / 0x28 (slave), identical to i686, so hardware IRQs land
    // on vectors 32..47 instead of clashing with CPU exceptions 0..31.
    port_outb(0x20, 0x11); port_outb(0xA0, 0x11);
    port_outb(0x21, 0x20); port_outb(0xA1, 0x28);
    port_outb(0x21, 0x04); port_outb(0xA1, 0x02);
    port_outb(0x21, 0x01); port_outb(0xA1, 0x01);
    port_outb(0x21, 0x00); port_outb(0xA1, 0x00);

    // CPU exceptions 0..31. 0x8E = present, DPL0, 64-bit interrupt gate. #DF (vector 8) uses
    // IST1 (the dedicated stack wired in Gdt64) so a corrupt rsp can't cascade to a triple
    // fault; the rest use IST0 (#PF reports fine via CR2 without a separate stack).
    setGate(0,  (uint64_t) isr0,  0x08, 0, 0x8E);
    setGate(1,  (uint64_t) isr1,  0x08, 0, 0x8E);
    setGate(2,  (uint64_t) isr2,  0x08, 0, 0x8E);
    setGate(3,  (uint64_t) isr3,  0x08, 0, 0x8E);
    setGate(4,  (uint64_t) isr4,  0x08, 0, 0x8E);
    setGate(5,  (uint64_t) isr5,  0x08, 0, 0x8E);
    setGate(6,  (uint64_t) isr6,  0x08, 0, 0x8E);
    setGate(7,  (uint64_t) isr7,  0x08, 0, 0x8E);
    setGate(8,  (uint64_t) isr8,  0x08, 1, 0x8E);   // #DF -> IST1
    setGate(9,  (uint64_t) isr9,  0x08, 0, 0x8E);
    setGate(10, (uint64_t) isr10, 0x08, 0, 0x8E);
    setGate(11, (uint64_t) isr11, 0x08, 0, 0x8E);
    setGate(12, (uint64_t) isr12, 0x08, 0, 0x8E);
    setGate(13, (uint64_t) isr13, 0x08, 0, 0x8E);   // #GP
    setGate(14, (uint64_t) isr14, 0x08, 0, 0x8E);   // #PF
    setGate(15, (uint64_t) isr15, 0x08, 0, 0x8E);
    setGate(16, (uint64_t) isr16, 0x08, 0, 0x8E);
    setGate(17, (uint64_t) isr17, 0x08, 0, 0x8E);
    setGate(18, (uint64_t) isr18, 0x08, 0, 0x8E);
    setGate(19, (uint64_t) isr19, 0x08, 0, 0x8E);
    setGate(20, (uint64_t) isr20, 0x08, 0, 0x8E);
    setGate(21, (uint64_t) isr21, 0x08, 0, 0x8E);
    setGate(22, (uint64_t) isr22, 0x08, 0, 0x8E);
    setGate(23, (uint64_t) isr23, 0x08, 0, 0x8E);
    setGate(24, (uint64_t) isr24, 0x08, 0, 0x8E);
    setGate(25, (uint64_t) isr25, 0x08, 0, 0x8E);
    setGate(26, (uint64_t) isr26, 0x08, 0, 0x8E);
    setGate(27, (uint64_t) isr27, 0x08, 0, 0x8E);
    setGate(28, (uint64_t) isr28, 0x08, 0, 0x8E);
    setGate(29, (uint64_t) isr29, 0x08, 0, 0x8E);
    setGate(30, (uint64_t) isr30, 0x08, 0, 0x8E);
    setGate(31, (uint64_t) isr31, 0x08, 0, 0x8E);

    // Hardware IRQs 0..15 -> vectors 32..47.
    setGate(32, (uint64_t) irq0,  0x08, 0, 0x8E);
    setGate(33, (uint64_t) irq1,  0x08, 0, 0x8E);
    setGate(34, (uint64_t) irq2,  0x08, 0, 0x8E);
    setGate(35, (uint64_t) irq3,  0x08, 0, 0x8E);
    setGate(36, (uint64_t) irq4,  0x08, 0, 0x8E);
    setGate(37, (uint64_t) irq5,  0x08, 0, 0x8E);
    setGate(38, (uint64_t) irq6,  0x08, 0, 0x8E);
    setGate(39, (uint64_t) irq7,  0x08, 0, 0x8E);
    setGate(40, (uint64_t) irq8,  0x08, 0, 0x8E);
    setGate(41, (uint64_t) irq9,  0x08, 0, 0x8E);
    setGate(42, (uint64_t) irq10, 0x08, 0, 0x8E);
    setGate(43, (uint64_t) irq11, 0x08, 0, 0x8E);
    setGate(44, (uint64_t) irq12, 0x08, 0, 0x8E);
    setGate(45, (uint64_t) irq13, 0x08, 0, 0x8E);
    setGate(46, (uint64_t) irq14, 0x08, 0, 0x8E);
    setGate(47, (uint64_t) irq15, 0x08, 0, 0x8E);

    // MSI/MSI-X vectors 0x70..0x77 -> the irq_msiN stubs (irq64.S). LAPIC-delivered; irq_handler
    // EOI's these via the LAPIC, not the PIC. knx_register_msi allocates one for the NIC.
    setGate(0x70, (uint64_t) irq_msi0, 0x08, 0, 0x8E);
    setGate(0x71, (uint64_t) irq_msi1, 0x08, 0, 0x8E);
    setGate(0x72, (uint64_t) irq_msi2, 0x08, 0, 0x8E);
    setGate(0x73, (uint64_t) irq_msi3, 0x08, 0, 0x8E);
    setGate(0x74, (uint64_t) irq_msi4, 0x08, 0, 0x8E);
    setGate(0x75, (uint64_t) irq_msi5, 0x08, 0, 0x8E);
    setGate(0x76, (uint64_t) irq_msi6, 0x08, 0, 0x8E);
    setGate(0x77, (uint64_t) irq_msi7, 0x08, 0, 0x8E);

    ptr.limit = sizeof(entries) - 1;
    ptr.base  = (uint64_t) &entries;
    __asm__ __volatile__("lidt %0" : : "m"(ptr));
}

}  // namespace kernel
