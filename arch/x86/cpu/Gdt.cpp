#include "Gdt.h"


namespace kernel {

void Gdt::initialize() {
	gdtPtr.limit = sizeof(GdtEntry) * 6 - 1;
	gdtPtr.base = (unsigned) &gdtEntries;

	// Flat memory model: each segment spans the full 4 GiB address space.
	// The selectors below MUST match what loader.s' gdt_flush and the IDT
	// gates assume: 0x08 = code, 0x10 = data.
	setGate(0, 0, 0, 0, 0);                   // null descriptor
	setGate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);    // 0x08: ring-0 code
	setGate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);    // 0x10: ring-0 data
	setGate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);    // 0x1B: ring-3 code (DPL=3)
	setGate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);    // 0x23: ring-3 data (DPL=3)

	// 0x28: TSS — system descriptor, base = &m_tss, limit = sizeof-1, access 0x89
	// (present, 32-bit available TSS, DPL=0). m_tss is zeroed (.bss); set ss0 and
	// disable the I/O bitmap. esp0 is filled by setKernelStack before ring 3.
	m_tss.ss0 = 0x10;
	m_tss.iomap_base = sizeof(Tss);
	setGate(5, (unsigned) &m_tss, sizeof(Tss) - 1, 0x89, 0x00);

	gdt_flush((unsigned) &gdtPtr);
}

void Gdt::setKernelStack(unsigned esp0) {
	m_tss.esp0 = esp0;
}

void Gdt::loadTss() {
	__asm__ __volatile__("ltr %0" : : "r"((unsigned short) 0x28));
}

void Gdt::setGate(int num, unsigned base, unsigned limit, unsigned char access,
		unsigned char gran) {
	gdtEntries[num].base_lo = base & 0xFFFF;
	gdtEntries[num].base_mid = (base >> 16) & 0xFF;
	gdtEntries[num].base_hi = (base >> 24) & 0xFF;

	gdtEntries[num].limit_lo = limit & 0xFFFF;
	gdtEntries[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);

	gdtEntries[num].access = access;
}

}
