/* linuxkpi/include/linux/timekeeping.h — monotonic time from the kernel uptime export. */
#ifndef _LINUXKPI_LINUX_TIMEKEEPING_H
#define _LINUXKPI_LINUX_TIMEKEEPING_H
#include <linux/types.h>
#include <linux/ktime.h>
unsigned long long knx_uptime_us(void);
/* ktime_get()/ktime_get_boottime() live in the shim's ktime.h */
/* ktime_get_raw() AND ktime_get_raw_ns() live in ktime.h (their natural home, alongside
 * ktime_get); reached here via the include above. i915_utils.h pulls ktime.h directly (not
 * timekeeping.h), which is why they must live there — a copy here would double-define for any
 * file that includes both (intel_dp, intel_guc_ct). */
static inline u64 ktime_get_ns(void){ return knx_uptime_us()*1000ull; }
static inline u64 ktime_get_mono_fast_ns(void){ return ktime_get_ns(); }
#ifndef _LKPI_KTIME_BOOTTIME_NS
#define _LKPI_KTIME_BOOTTIME_NS
static inline u64 ktime_get_boottime_ns(void){ return ktime_get_ns(); }
#endif
static inline void ktime_get_ts64(struct timespec64 *ts){ u64 ns=ktime_get_ns(); ts->tv_sec=ns/1000000000ull; ts->tv_nsec=ns%1000000000ull; }
#endif
