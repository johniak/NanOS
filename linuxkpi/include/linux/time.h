/* linuxkpi/include/linux/time.h — forwards to ktime/jiffies time helpers already in the shim. */
#ifndef _LINUXKPI_LINUX_TIME_H
#define _LINUXKPI_LINUX_TIME_H
#include <linux/types.h>
#include <linux/ktime.h>
#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC  1000000000L
#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_USEC 1000L
#define USEC_PER_SEC  1000000L
#endif
#endif
