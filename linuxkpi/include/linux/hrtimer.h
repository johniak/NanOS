/*
 * linuxkpi/include/linux/hrtimer.h — minimal high-res timer surface for the shim.
 * virtio_ring only uses ktime for its used-ring poll timeout; full hrtimers are not wired.
 */
#ifndef _LINUXKPI_LINUX_HRTIMER_H
#define _LINUXKPI_LINUX_HRTIMER_H

#include <linux/types.h>
#include <linux/ktime.h>

enum hrtimer_mode { HRTIMER_MODE_REL = 0, HRTIMER_MODE_ABS = 1, HRTIMER_MODE_REL_PINNED = 2, HRTIMER_MODE_ABS_PINNED = 3, HRTIMER_MODE_REL_HARD = 4, HRTIMER_MODE_ABS_HARD = 5 };
enum hrtimer_restart { HRTIMER_NORESTART = 0, HRTIMER_RESTART = 1 };

struct hrtimer {
	ktime_t _softexpires;
	enum hrtimer_restart (*function)(struct hrtimer *);
};

static inline void hrtimer_init(struct hrtimer *t, int which, enum hrtimer_mode mode) { (void)which;(void)mode; t->function = 0; }
static inline int  hrtimer_cancel(struct hrtimer *t) { (void)t; return 0; }
static inline int  hrtimer_try_to_cancel(struct hrtimer *t) { (void)t; return 0; }
static inline void hrtimer_start(struct hrtimer *t, ktime_t tim, enum hrtimer_mode mode) { (void)t;(void)tim;(void)mode; }
static inline void hrtimer_start_range_ns(struct hrtimer *t, ktime_t tim, u64 range_ns, enum hrtimer_mode mode) { (void)t;(void)tim;(void)range_ns;(void)mode; }
static inline u64 hrtimer_forward(struct hrtimer *t, ktime_t now, ktime_t interval) { (void)t;(void)now;(void)interval; return 0; }
static inline u64 hrtimer_forward_now(struct hrtimer *t, ktime_t interval) { (void)t;(void)interval; return 0; }
static inline bool hrtimer_active(const struct hrtimer *t) { (void)t; return false; }
static inline void hrtimer_setup(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *), int which, enum hrtimer_mode mode) { (void)which;(void)mode; t->function = fn; }

#endif /* _LINUXKPI_LINUX_HRTIMER_H */
