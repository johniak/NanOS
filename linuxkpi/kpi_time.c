/*
 * linuxkpi/kpi_time.c — jiffies clock + delays for the LinuxKPI shim.
 *
 * jiffies is derived from the kernel monotonic microsecond clock (knx_uptime_us) at
 * HZ=1000, so 1 jiffy == 1 ms. Short delays busy-wait on the same clock; msleep yields
 * (knx_yield if available, else busy-wait) so it does not hard-spin a CPU.
 */
#include <linux/jiffies.h>
#include <linux/delay.h>

extern unsigned long long knx_uptime_us(void);
#ifndef NANOS_HOST_TEST
extern void knx_yield(void) __attribute__((weak));
#endif

unsigned long lkpi_jiffies(void) {
	/* us -> ms (HZ=1000) */
	return (unsigned long)(knx_uptime_us() / 1000ull);
}

void udelay(unsigned long usecs) {
	unsigned long long start = knx_uptime_us();
	while (knx_uptime_us() - start < (unsigned long long)usecs)
		;
}

void ndelay(unsigned long nsecs) {
	udelay((nsecs + 999) / 1000);
}

void mdelay(unsigned long msecs) {
	udelay(msecs * 1000ul);
}

void msleep(unsigned int msecs) {
	unsigned long long start = knx_uptime_us();
	unsigned long long end = start + (unsigned long long)msecs * 1000ull;
	while (knx_uptime_us() < end) {
#ifndef NANOS_HOST_TEST
		if (knx_yield)
			knx_yield();
#endif
	}
}

unsigned long msleep_interruptible(unsigned int msecs) {
	msleep(msecs);
	return 0;
}

void usleep_range(unsigned long min, unsigned long max) {
	(void)max;
	udelay(min);
}

void usleep_range_state(unsigned long min, unsigned long max, unsigned int state) {
	(void)state;
	usleep_range(min, max);
}
