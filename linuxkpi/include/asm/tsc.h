/* linuxkpi/include/asm/tsc.h — rdtsc(). */
#ifndef _LKPI_ASM_TSC_H
#define _LKPI_ASM_TSC_H
#include <linux/types.h>
static inline u64 rdtsc(void)
{
	u32 lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}
static inline u64 rdtsc_ordered(void) { return rdtsc(); }
#endif
