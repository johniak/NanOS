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
	// The LAPIC MMIO register page (base & ~0xFFF, default 0xFEE00000) is mapped by mmuInitKernel
	// (it sits above RAM, outside the huge identity map). Do NOT map it here — a late page-table
	// allocation at cpuInit time corrupts the live kernel stack.
	g_lapic = (volatile uint32_t*) (uintptr_t) (base & 0xFFFFF000ull);
	g_lapic[LAPIC_SPURIOUS / 4] = 0x100 | 0xFF;            // bit8 = APIC software enable, spurious vec 0xFF
	msiVecPoolInit(&g_pool);
}
uint8_t lapicId()  { return (uint8_t) (g_lapic[LAPIC_ID / 4] >> 24); }
void    lapicEoi() { g_lapic[LAPIC_EOI / 4] = 0; }
int     lapicAllocVector() { return msiVecAlloc(&g_pool); }

// Interrupt Command Register: a 64-bit command split across ICR_HI (destination, bits 24-27)
// and ICR_LO (delivery mode/vector). Writing the LOW word triggers the send; bit 12 of ICR_LO
// is Delivery Status — spin until it clears (= idle) before issuing the next IPI.
enum { LAPIC_ICR_LO = 0x300, LAPIC_ICR_HI = 0x310,
       LAPIC_LVT_TIMER = 0x320, LAPIC_TIMER_INIT = 0x380,
       LAPIC_TIMER_CUR = 0x390, LAPIC_TIMER_DIV = 0x3E0 };

static void icrWrite(uint8_t dest, uint32_t lo) {
	g_lapic[LAPIC_ICR_HI / 4] = (uint32_t) dest << 24;
	g_lapic[LAPIC_ICR_LO / 4] = lo;
	while (g_lapic[LAPIC_ICR_LO / 4] & (1u << 12)) { }   // wait for Delivery Status = idle
}
// Delivery encodings (Intel SDM Vol.3 §10.6): 0x4500 = INIT/assert/edge, 0x4600|vec = STARTUP
// (SIPI), 0x4000|vec = fixed delivery.
void lapicSendInit(uint8_t dest)               { icrWrite(dest, 0x4500); }
void lapicSendStartup(uint8_t dest, uint8_t v) { icrWrite(dest, 0x4600 | v); }
void lapicSendFixed(uint8_t dest, uint8_t v)   { icrWrite(dest, 0x4000 | v); }

void lapicTimerInit(uint8_t vec, uint32_t init) {
	g_lapic[LAPIC_TIMER_DIV / 4]  = 0x3;                       // divide configuration: by 16
	g_lapic[LAPIC_LVT_TIMER / 4]  = (uint32_t) vec | (1u << 17);   // periodic mode (bit 17)
	g_lapic[LAPIC_TIMER_INIT / 4] = init;                     // initial count -> counts down
}

static inline unsigned char calInb(unsigned short p) {
	unsigned char v; __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p)); return v;
}
static inline void calOutb(unsigned short p, unsigned char v) {
	__asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(p));
}

// Measure the LAPIC timer rate (divide-by-16) against PIT channel 2 over ~10 ms, returning the
// LAPIC tick count for ONE millisecond. lapicTimerInit(vec, that) then fires at ~1000 Hz. The
// LAPIC bus clock is the same on every CPU, so the BSP calibrates once and all CPUs reuse it.
uint32_t lapicTimerCalibrate() {
	const unsigned PIT_HZ = 1193182u, MS = 10u;
	unsigned count = PIT_HZ * MS / 1000u;
	g_lapic[LAPIC_TIMER_DIV / 4] = 0x3;                       // divide by 16
	// Arm PIT channel 2 (speaker timer — NOT the system tick on ch0) for a one-shot ~10 ms.
	calOutb(0x61, (unsigned char) ((calInb(0x61) & ~0x02) | 0x01));
	calOutb(0x43, 0xB0);                                      // ch2, lo/hi byte, mode 0 (one-shot)
	calOutb(0x42, (unsigned char) (count & 0xFF));
	calOutb(0x42, (unsigned char) ((count >> 8) & 0xFF));
	unsigned char g = (unsigned char) (calInb(0x61) & ~0x01);
	calOutb(0x61, g);
	calOutb(0x61, (unsigned char) (g | 0x01));                // restart the gate -> begins counting
	g_lapic[LAPIC_TIMER_INIT / 4] = 0xFFFFFFFFu;              // start the LAPIC countdown
	while (!(calInb(0x61) & 0x20)) { }                        // wait for ch2 terminal count
	uint32_t elapsed = 0xFFFFFFFFu - g_lapic[LAPIC_TIMER_CUR / 4];
	g_lapic[LAPIC_TIMER_INIT / 4] = 0;                        // stop
	uint32_t perMs = elapsed / MS;
	return perMs ? perMs : 1;
}

#endif  // NANOS_HOST_TEST

}  // namespace kernel
