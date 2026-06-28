/*
 * linuxkpi/include/linux/hrtimer.h — minimal high-res timer surface for the shim.
 * virtio_ring only uses ktime for its used-ring poll timeout; full hrtimers are not wired.
 */
#ifndef _LINUXKPI_LINUX_HRTIMER_H
#define _LINUXKPI_LINUX_HRTIMER_H

#include <linux/types.h>
#include <linux/ktime.h>

enum hrtimer_mode { HRTIMER_MODE_REL = 0, HRTIMER_MODE_ABS = 1 };
enum hrtimer_restart { HRTIMER_NORESTART = 0, HRTIMER_RESTART = 1 };

struct hrtimer {
	ktime_t _softexpires;
	enum hrtimer_restart (*function)(struct hrtimer *);
};

static inline void hrtimer_init(struct hrtimer *t, int which, enum hrtimer_mode mode) { (void)which;(void)mode; t->function = 0; }
static inline int  hrtimer_cancel(struct hrtimer *t) { (void)t; return 0; }
static inline void hrtimer_start(struct hrtimer *t, ktime_t tim, enum hrtimer_mode mode) { (void)t;(void)tim;(void)mode; }

#endif /* _LINUXKPI_LINUX_HRTIMER_H */
