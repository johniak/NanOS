/*
 * linuxkpi/include/linux/iopoll.h — read-poll-timeout helpers (i915 display: PLL lock, DP aux, PHY
 * ready). Busy-polls the accessor until the condition holds or the microsecond budget elapses,
 * timing out with -ETIMEDOUT. The budget uses the monotonic clock (local_clock, ns). `sleep_us` is
 * advisory (we busy-spin — the bring-up path is cooperative and these waits are short).
 */
#ifndef _LINUXKPI_LINUX_IOPOLL_H
#define _LINUXKPI_LINUX_IOPOLL_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched/clock.h>

#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before, args...) \
({ \
	u64 __end = local_clock() + (u64)(timeout_us) * 1000ull; \
	int __ret = 0; \
	for (;;) { \
		(val) = (op)(args); \
		if (cond) { __ret = 0; break; } \
		if ((timeout_us) && local_clock() >= __end) { \
			(val) = (op)(args); \
			__ret = (cond) ? 0 : -ETIMEDOUT; \
			break; \
		} \
	} \
	__ret; \
})

#define readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us) \
	read_poll_timeout(op, val, cond, sleep_us, timeout_us, false, addr)

#define readb_poll_timeout(addr, val, cond, sleep_us, timeout_us) \
	readx_poll_timeout(readb, addr, val, cond, sleep_us, timeout_us)
#define readw_poll_timeout(addr, val, cond, sleep_us, timeout_us) \
	readx_poll_timeout(readw, addr, val, cond, sleep_us, timeout_us)
#define readl_poll_timeout(addr, val, cond, sleep_us, timeout_us) \
	readx_poll_timeout(readl, addr, val, cond, sleep_us, timeout_us)
#define readl_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout(readl, addr, val, cond, delay_us, timeout_us)

#endif /* _LINUXKPI_LINUX_IOPOLL_H */
