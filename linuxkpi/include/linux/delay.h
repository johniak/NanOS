/*
 * linuxkpi/include/linux/delay.h — busy/sleep delays for the LinuxKPI shim (kpi_time.c).
 * Short delays busy-wait on knx_uptime_us(); msleep yields to the scheduler.
 */
#ifndef _LINUXKPI_LINUX_DELAY_H
#define _LINUXKPI_LINUX_DELAY_H

#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void udelay(unsigned long usecs);
void ndelay(unsigned long nsecs);
void mdelay(unsigned long msecs);
void msleep(unsigned int msecs);
unsigned long msleep_interruptible(unsigned int msecs);
void usleep_range(unsigned long min, unsigned long max);
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_DELAY_H */
