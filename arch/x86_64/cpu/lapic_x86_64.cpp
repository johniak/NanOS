#include "lapic_x86_64.h"
#include <stdint.h>

namespace kernel {

// ---- pure-logic vector pool (host-tested) ----
void msiVecPoolInit(MsiVecPool* p) { p->next = MSI_VEC_BASE; }
int  msiVecAlloc(MsiVecPool* p) {
	if (p->next >= MSI_VEC_END)
		return -1;
	return (int) p->next++;
}

#ifndef NANOS_HOST_TEST    // the LAPIC MMIO/MSR half is kernel-only

static volatile uint32_t* g_lapic = 0;     // identity-mapped LAPIC MMIO base
static MsiVecPool g_pool;

static inline uint64_t rdmsr(uint32_t msr) {
	uint32_t lo, hi;
	__asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t) hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
	__asm__ __volatile__("wrmsr" :: "c"(msr), "a"((uint32_t) v), "d"((uint32_t)(v >> 32)));
}

enum { IA32_APIC_BASE = 0x1B };
enum { LAPIC_ID = 0x20, LAPIC_EOI = 0xB0, LAPIC_SPURIOUS = 0xF0 };

void lapicInit() {
	uint64_t base = rdmsr(IA32_APIC_BASE);
	base |= (1ull << 11);                                  // global LAPIC enable
	wrmsr(IA32_APIC_BASE, base);
	g_lapic = (volatile uint32_t*) (uintptr_t) (base & 0xFFFFF000ull);
	g_lapic[LAPIC_SPURIOUS / 4] = 0x100 | 0xFF;            // bit8 = APIC software enable, spurious vec 0xFF
	msiVecPoolInit(&g_pool);
}
uint8_t lapicId()  { return (uint8_t) (g_lapic[LAPIC_ID / 4] >> 24); }
void    lapicEoi() { g_lapic[LAPIC_EOI / 4] = 0; }
int     lapicAllocVector() { return msiVecAlloc(&g_pool); }

#endif  // NANOS_HOST_TEST

}  // namespace kernel
