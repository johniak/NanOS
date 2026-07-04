/* linuxkpi/include/linux/time64.h — timespec64 lives in the shim's ktime.h; this adds the ns
 * conversion helpers GuC CT logging uses. */
#ifndef _LINUXKPI_LINUX_TIME64_H
#define _LINUXKPI_LINUX_TIME64_H
#include <linux/types.h>
#include <linux/ktime.h>   /* struct timespec64 */
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000L
#endif
static inline s64 timespec64_to_ns(const struct timespec64 *ts){ return ts->tv_sec*1000000000ll + ts->tv_nsec; }
static inline struct timespec64 ns_to_timespec64(s64 ns){ struct timespec64 t; t.tv_sec=ns/1000000000ll; t.tv_nsec=ns%1000000000ll; return t; }
#endif
