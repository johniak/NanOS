/*
 * linuxkpi/include/linux/sched/clock.h — local_clock()/sched_clock() as a monotonic ns counter,
 * sourced from the kernel uptime export (knx_uptime_us). i915 uses it for timestamps/heartbeat
 * bookkeeping, not for anything requiring true per-CPU cycle accuracy.
 */
#ifndef _LINUXKPI_LINUX_SCHED_CLOCK_H
#define _LINUXKPI_LINUX_SCHED_CLOCK_H
#include <linux/types.h>
unsigned long long knx_uptime_us(void);
static inline u64 local_clock(void)  { return knx_uptime_us() * 1000ull; }
static inline u64 sched_clock(void)   { return local_clock(); }
static inline u64 cpu_clock(int cpu)  { (void)cpu; return local_clock(); }
static inline u64 local_clock_noinstr(void) { return local_clock(); }
#endif
