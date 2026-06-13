/*
 * random_x86.cpp — x86 implementation of <arch/random.h>.
 *
 * Hardware entropy for the kernel CSPRNG seed: RDRAND (an on-die DRBG fed by a thermal-noise
 * entropy source, present from Ivy Bridge on) when CPUID advertises it, plus RDTSC as the
 * jitter counter. The default QEMU "qemu32" CPU does NOT expose RDRAND — `make run` therefore
 * passes `-cpu Nehalem` so QEMU surfaces it; without that flag archHwRandom returns false and
 * the seeder falls back to RDTSC jitter + RTC alone (documented, software-only entropy).
 */
#include <arch/random.h>

namespace {
inline void cpuid(unsigned leaf, unsigned* a, unsigned* b, unsigned* c, unsigned* d) {
	__asm__ __volatile__("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

// CPUID leaf 1, ECX bit 30 = RDRAND supported. Computed once, cached (CPUID is not cheap and
// the answer never changes during a boot).
bool hasRdrand() {
	static int cached = -1;
	if (cached < 0) {
		unsigned a, b, c, d;
		cpuid(1, &a, &b, &c, &d);
		cached = (c & (1u << 30)) ? 1 : 0;
	}
	return cached != 0;
}

inline unsigned long long rdtsc() {
	unsigned lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long) hi << 32) | lo;
}
}

namespace arch {

bool archHwRandom(unsigned* out) {
	if (!hasRdrand())
		return false;
	// RDRAND sets CF=1 when the value is valid; retry a bounded number of times on the rare
	// "not ready" (CF=0). The Intel guidance is 10 retries before declaring failure.
	for (int tries = 0; tries < 10; tries++) {
		unsigned val;
		unsigned char ok;
		__asm__ __volatile__("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
		if (ok) {
			*out = val;
			return true;
		}
	}
	return false;
}

unsigned archEntropyTick() {
	return (unsigned) rdtsc();
}

}
