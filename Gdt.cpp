#include "Gdt.h"


namespace kernel {

void Gdt::initialize() {
	gdtPtr.limit = sizeof(GdtEntry) * 3 - 1;
	gdtPtr.base = (unsigned) &gdtEntries;

	// Flat memory model: each segment spans the full 4 GiB address space.
	// The selectors below MUST match what loader.s' gdt_flush and the IDT
	// gates assume: 0x08 = code, 0x10 = data.
	setGate(0, 0, 0, 0, 0);                   // null descriptor
	setGate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);    // 0x08: ring-0 code
	setGate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);    // 0x10: ring-0 data

	gdt_flush((unsigned) &gdtPtr);
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
