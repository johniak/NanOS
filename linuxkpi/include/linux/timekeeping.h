/* linuxkpi/include/linux/timekeeping.h — monotonic time from the kernel uptime export. */
#ifndef _LINUXKPI_LINUX_TIMEKEEPING_H
#define _LINUXKPI_LINUX_TIMEKEEPING_H
#include <linux/types.h>
#include <linux/ktime.h>
unsigned long long knx_uptime_us(void);
/* ktime_get()/ktime_get_boottime() live in the shim's ktime.h */
static inline u64 ktime_get_raw_ns(void){ return knx_uptime_us()*1000ull; }
static inline ktime_t ktime_get_raw(void){ return (ktime_t)(knx_uptime_us()*1000ull); }
static inline u64 ktime_get_ns(void){ return knx_uptime_us()*1000ull; }
static inline u64 ktime_get_mono_fast_ns(void){ return ktime_get_ns(); }
static inline u64 ktime_get_boottime_ns(void){ return ktime_get_ns(); }
static inline void ktime_get_ts64(struct timespec64 *ts){ u64 ns=ktime_get_ns(); ts->tv_sec=ns/1000000000ull; ts->tv_nsec=ns%1000000000ull; }
#endif
