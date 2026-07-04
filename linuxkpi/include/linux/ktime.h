/*
 * linuxkpi/include/linux/ktime.h — ktime (nanosecond monotonic) for the shim, from the
 * kernel monotonic microsecond clock (knx_uptime_us).
 */
#ifndef _LINUXKPI_LINUX_KTIME_H
#define _LINUXKPI_LINUX_KTIME_H

#include <linux/types.h>

typedef s64 ktime_t;

#define NSEC_PER_SEC   1000000000LL
#define NSEC_PER_MSEC  1000000LL
#define NSEC_PER_USEC  1000LL
#define USEC_PER_SEC   1000000LL
#define MSEC_PER_SEC   1000LL

extern unsigned long long knx_uptime_us(void);

static inline ktime_t ktime_get(void) { return (ktime_t)(knx_uptime_us() * 1000ull); }
static inline ktime_t ktime_get_boottime(void) { return ktime_get(); }
#ifndef _LKPI_KTIME_BOOTTIME_NS
#define _LKPI_KTIME_BOOTTIME_NS
static inline u64 ktime_get_boottime_ns(void) { return (u64)ktime_get_boottime(); }
#endif
/* compose a ktime from seconds + nanoseconds (ktime_t is a scalar ns count here). */
static inline ktime_t ktime_set(s64 secs, unsigned long nsecs) { return secs*1000000000LL + (s64)nsecs; }
#ifndef KTIME_MAX
#define KTIME_MAX ((s64)~((u64)1 << 63))
#define KTIME_SEC_MAX (KTIME_MAX / 1000000000LL)
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_BOOTTIME  7
#endif
/* Raw (NTP-uncorrected) monotonic clock. We have no NTP discipline, so it equals ktime_get(). i915
 * uses it for engine-busy timestamps. */
static inline ktime_t ktime_get_raw(void) { return ktime_get(); }
/* ns form of the raw monotonic clock (intel_rps IPS timestamps); ktime_t is already ns here. */
static inline u64 ktime_get_raw_ns(void) { return (u64)ktime_get_raw(); }
/* Lock-free fast monotonic-raw read in ns (i915 uses it on the context-switch hot path). No seqlock
 * to bypass here — the uptime read is already a single load. */
static inline u64 ktime_get_raw_fast_ns(void) { return knx_uptime_us() * 1000ull; }
static inline s64 ktime_to_ns(ktime_t k) { return k; }
static inline s64 ktime_to_us(ktime_t k) { return k / 1000; }
static inline s64 ktime_to_ms(ktime_t k) { return k / 1000000; }
static inline ktime_t ktime_add(ktime_t a, ktime_t b) { return a + b; }
static inline ktime_t ktime_add_ns(ktime_t a, u64 ns) { return a + (s64)ns; }
static inline ktime_t ktime_add_us(ktime_t a, u64 us) { return a + (s64)us * 1000; }
static inline ktime_t ktime_add_ms(ktime_t a, u64 ms) { return a + (s64)ms * 1000000; }
static inline ktime_t ktime_sub(ktime_t a, ktime_t b) { return a - b; }
static inline s64 ktime_us_delta(ktime_t a, ktime_t b) { return ktime_to_us(a - b); }
static inline s64 ktime_ms_delta(ktime_t a, ktime_t b) { return ktime_to_ms(a - b); }
static inline int ktime_compare(ktime_t a, ktime_t b) { return (a < b) ? -1 : (a > b) ? 1 : 0; }
static inline bool ktime_after(ktime_t a, ktime_t b) { return ktime_compare(a, b) > 0; }
static inline bool ktime_before(ktime_t a, ktime_t b) { return ktime_compare(a, b) < 0; }
static inline ktime_t ns_to_ktime(u64 ns) { return (ktime_t)ns; }
static inline ktime_t us_to_ktime(u64 us) { return (ktime_t)us * 1000; }
static inline ktime_t ms_to_ktime(u64 ms) { return (ktime_t)ms * 1000000; }

#endif /* _LINUXKPI_LINUX_KTIME_H */

#ifndef _LKPI_KTIME_X
#define _LKPI_KTIME_X
static inline ktime_t ktime_sub_ns(ktime_t k, u64 ns){ return k-(s64)ns; }
struct timespec64 { s64 tv_sec; long tv_nsec; };
static inline struct timespec64 ktime_to_timespec64(ktime_t k){ struct timespec64 t; t.tv_sec=k/1000000000LL; t.tv_nsec=k%1000000000LL; return t; }
#endif
