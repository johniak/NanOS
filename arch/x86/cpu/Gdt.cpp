#include "Gdt.h"
#include "GdtBase.h"


namespace kernel {

void Gdt::initialize() {
	gdtPtr.limit = sizeof(GdtEntry) * 7 - 1;
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

	// 0x33: TLS — one ring-3 (DPL=3) data segment, same flags as 0x23, base 0 for now.
	// The compiler emits __thread accesses as %gs:-relative (i686 variant II); the scheduler
	// re-points this descriptor's base at the current thread's TLS block on every switch via
	// setTlsBase (selector (6<<3)|3 == 0x33). One reloaded entry suffices on a uniprocessor.
	setGate(6, 0, 0xFFFFFFFF, 0xF2, 0xCF);

	gdt_flush((unsigned) &gdtPtr);
}

void Gdt::setTlsBase(unsigned base) {
	// Rewrite only entry 6's base bytes, then reload %gs (0x33) so the CPU refreshes the
	// hidden descriptor cache from the updated entry. Base 0 (a thread with no TLS) is
	// harmless: the kernel never touches %gs and such a thread never reads %gs:-relative.
	// Base bytes only (limit/flags untouched) — mirrors gdtPackBase's base scatter; the hot path
	// must not rewrite the limit. The "memory" clobber serializes these stores BEFORE the %gs
	// reload: without it, at -O2 the compiler could hoist the segment load ahead of the base
	// writes and reload %gs from a stale base.
	unsigned char* d = (unsigned char*) &gdtEntries[6];
	d[2] = (unsigned char) (base & 0xFF);
	d[3] = (unsigned char) ((base >> 8) & 0xFF);
	d[4] = (unsigned char) ((base >> 16) & 0xFF);
	d[7] = (unsigned char) ((base >> 24) & 0xFF);
	__asm__ __volatile__("mov %0, %%gs" : : "r"((unsigned short) 0x33) : "memory");
}

void Gdt::setKernelStack(unsigned esp0) {
	m_tss.esp0 = esp0;
}

void Gdt::loadTss() {
	__asm__ __volatile__("ltr %0" : : "r"((unsigned short) 0x28));
}

void Gdt::setGate(int num, unsigned base, unsigned limit, unsigned char access,
		unsigned char gran) {
	// Base + 20-bit limit go through the host-tested pure packer; access and the high
	// flag nibble (granularity/size) are this descriptor's own bits.
	unsigned char* d = (unsigned char*) &gdtEntries[num];
	kernel_arch::gdtPackBase(d, base, limit);
	gdtEntries[num].granularity = (unsigned char) ((gdtEntries[num].granularity & 0x0F) | (gran & 0xF0));
	gdtEntries[num].access = access;
}

}
