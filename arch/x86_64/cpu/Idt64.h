/*
 * Idt64.h — x86-64 Interrupt Descriptor Table.
 *
 * Each gate is 16 bytes: a 64-bit handler base split into lo/mid/hi, plus a 3-bit IST index
 * (0 = use the stack the CPU would normally pick; 1..7 = switch to TSS.istN). flags = type +
 * DPL + present (0x8E = present, DPL0, 64-bit interrupt gate, which also clears IF on entry).
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct IdtEntry64 {
    uint16_t base_lo;     // handler bits 0:15
    uint16_t sel;         // code selector (0x08)
    uint8_t  ist;         // bits 0:2 = IST index, bits 3:7 = 0
    uint8_t  flags;       // P | DPL | type
    uint16_t base_mid;    // handler bits 16:31
    uint32_t base_hi;     // handler bits 32:63
    uint32_t reserved;
} __attribute__((packed));

struct Idt64Ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

class Idt64 {
    IdtEntry64 entries[256];
    Idt64Ptr   ptr;
public:
    void initialize();    // remap PIC, install all gates, lidt
private:
    void setGate(unsigned char num, uint64_t handler, uint16_t sel, uint8_t ist, uint8_t flags);
};

}  // namespace kernel

// CPU-exception stubs (isr64.S) and device-IRQ stubs (irq64.S). The int 0x80 / syscall gate
// is deliberately absent — userspace + syscalls arrive in Plan 6.
extern "C" void isr0();  extern "C" void isr1();  extern "C" void isr2();  extern "C" void isr3();
extern "C" void isr4();  extern "C" void isr5();  extern "C" void isr6();  extern "C" void isr7();
extern "C" void isr8();  extern "C" void isr9();  extern "C" void isr10(); extern "C" void isr11();
extern "C" void isr12(); extern "C" void isr13(); extern "C" void isr14(); extern "C" void isr15();
extern "C" void isr16(); extern "C" void isr17(); extern "C" void isr18(); extern "C" void isr19();
extern "C" void isr20(); extern "C" void isr21(); extern "C" void isr22(); extern "C" void isr23();
extern "C" void isr24(); extern "C" void isr25(); extern "C" void isr26(); extern "C" void isr27();
extern "C" void isr28(); extern "C" void isr29(); extern "C" void isr30(); extern "C" void isr31();
extern "C" void irq0();  extern "C" void irq1();  extern "C" void irq2();  extern "C" void irq3();
extern "C" void irq4();  extern "C" void irq5();  extern "C" void irq6();  extern "C" void irq7();
extern "C" void irq8();  extern "C" void irq9();  extern "C" void irq10(); extern "C" void irq11();
extern "C" void irq12(); extern "C" void irq13(); extern "C" void irq14(); extern "C" void irq15();
// MSI/MSI-X vector stubs (irq64.S), vectors 0x70..0x77.
extern "C" void irq_msi0(); extern "C" void irq_msi1(); extern "C" void irq_msi2(); extern "C" void irq_msi3();
extern "C" void irq_msi4(); extern "C" void irq_msi5(); extern "C" void irq_msi6(); extern "C" void irq_msi7();
