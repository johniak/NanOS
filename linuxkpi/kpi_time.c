/*
 * linuxkpi/kpi_time.c — jiffies clock + delays for the LinuxKPI shim.
 *
 * jiffies is derived from the kernel monotonic microsecond clock (knx_uptime_us) at
 * HZ=1000, so 1 jiffy == 1 ms. Short delays busy-wait on the same clock; msleep yields
 * (knx_yield if available, else busy-wait) so it does not hard-spin a CPU.
 */
#include <linux/jiffies.h>
#include <linux/delay.h>
#include "lkpi_knx.h"

/* The sleeping delays (msleep/usleep_range) run in might_sleep context, so they PUMP the cooperative
 * work sources each turn (lkpi_wait_pump = virtio vq + timers + workqueue drain). Without this, an
 * i915 poll loop written as `while (!cond) msleep(1)` busy-waits real time but never services the
 * timer/workqueue whose callback sets `cond` — a silent starvation identical to the wait_bit.h case.
 * udelay/mdelay deliberately do NOT pump: they are called under spin_lock_irqsave (atomic sections
 * with interrupts off), where running a work/timer callback would violate the caller's assumptions. */
void lkpi_wait_pump(void);

/* msleep currently busy-waits on the monotonic clock; a cooperative knx_yield export is
 * added in P1 (threads/workqueue) and wired in here then. */

unsigned long lkpi_jiffies(void) {
	/* us -> ms (HZ=1000) */
	return (unsigned long)(knx_uptime_us() / 1000ull);
}

void lkpi_cpu_relax_probe(void *ra);
void udelay(unsigned long usecs) {
	unsigned long long start = knx_uptime_us();
	/* Diagnostic: a timeout-less i915 poll that busy-waits with udelay (rather than cpu_relax) is
	 * localized here too — the raw-loop watchdog only fires when the SAME caller spins > 3s. */
	lkpi_cpu_relax_probe(__builtin_return_address(0));
	while (knx_uptime_us() - start < (unsigned long long)usecs)
		;
}

void ndelay(unsigned long nsecs) {
	udelay((nsecs + 999) / 1000);
}

void mdelay(unsigned long msecs) {
	udelay(msecs * 1000ul);
}

void lkpi_sleep_probe(void *ra);

void msleep(unsigned int msecs) {
	void *ra = __builtin_return_address(0);
	unsigned long long start = knx_uptime_us();
	unsigned long long end = start + (unsigned long long)msecs * 1000ull;
	while (knx_uptime_us() < end) {
		lkpi_wait_pump();   /* service timers/workqueue so poll-by-msleep loops make progress */
		lkpi_sleep_probe(ra);   /* name a `while(!cond) msleep()` that never terminates (>8s) */
	}
}

unsigned long msleep_interruptible(unsigned int msecs) {
	msleep(msecs);
	return 0;
}

void usleep_range(unsigned long min, unsigned long max) {
	/* might_sleep context: pump the cooperative work sources while waiting out `min` microseconds,
	 * so a poll-by-usleep loop does not starve the timer/workqueue that satisfies its condition
	 * (unlike udelay, which is atomic-section-safe and must not pump). */
	void *ra = __builtin_return_address(0);
	unsigned long long end = knx_uptime_us() + (unsigned long long)min;
	(void)max;
	while (knx_uptime_us() < end) {
		lkpi_wait_pump();
		lkpi_sleep_probe(ra);   /* name a poll-by-usleep_range loop that never terminates (>8s) */
	}
}

void usleep_range_state(unsigned long min, unsigned long max, unsigned int state) {
	(void)state;
	usleep_range(min, max);
}
