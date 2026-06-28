/*
 * linuxkpi/include/linux/jiffies.h — jiffies + time comparison for the LinuxKPI shim.
 * HZ=1000 (1 jiffy = 1 ms). `jiffies` is computed live from knx_uptime_us() on each read,
 * so drivers never need an explicit refresh. Comparisons use the signed-difference trick
 * so they remain correct across an unsigned-long wrap.
 */
#ifndef _LINUXKPI_LINUX_JIFFIES_H
#define _LINUXKPI_LINUX_JIFFIES_H

#include <linux/types.h>

#define HZ 1000

#ifdef __cplusplus
extern "C" {
#endif

unsigned long lkpi_jiffies(void);

#ifdef __cplusplus
}
#endif

#define jiffies (lkpi_jiffies())

/* signed-difference comparisons: correct across unsigned-long wraparound */
#define time_after(a, b)      ((long)((b) - (a)) < 0)
#define time_before(a, b)     time_after(b, a)
#define time_after_eq(a, b)   ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b)  time_after_eq(b, a)
#define time_in_range(a, b, c) (time_after_eq(a, b) && time_before_eq(a, c))

static inline unsigned long msecs_to_jiffies(unsigned int m) {
	return (unsigned long)m * HZ / 1000u;   /* HZ=1000 -> 1:1 */
}
static inline unsigned int jiffies_to_msecs(unsigned long j) {
	return (unsigned int)(j * 1000u / HZ);
}
static inline unsigned long usecs_to_jiffies(unsigned int u) {
	return ((unsigned long)u + (1000000u / HZ) - 1) / (1000000u / HZ);
}
static inline u64 jiffies_to_nsecs(unsigned long j) {
	return (u64)j * (1000000000ull / HZ);
}
static inline u64 nsecs_to_jiffies(u64 n) {
	return n / (1000000000ull / HZ);
}

#endif /* _LINUXKPI_LINUX_JIFFIES_H */
