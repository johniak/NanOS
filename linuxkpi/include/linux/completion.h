/*
 * linuxkpi/include/linux/completion.h — completions for the shim.
 *
 * Bring-up implementation: `done` is polled with a pause hint. complete() is IRQ-safe
 * (just a store). A blocking waitqueue-backed version can replace this later without
 * changing callers.
 */
#ifndef _LINUXKPI_LINUX_COMPLETION_H
#define _LINUXKPI_LINUX_COMPLETION_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/barrier.h>

/* wait_for_completion SPINS (no real blocking waitqueue). Pump deferred work each turn so a
 * completion signalled by a workqueue bottom half / timer is reached instead of starved, and probe a
 * stuck site — same rationale as <linux/wait_bit.h>. (Declared here: completion.h is included before
 * wait.h.) */
void lkpi_wait_pump(void);
void lkpi_spin_probe(void *ra);

struct completion { volatile unsigned int done; };

#define COMPLETION_INITIALIZER(work) { 0 }
#define DECLARE_COMPLETION(work) struct completion work = COMPLETION_INITIALIZER(work)

static inline void init_completion(struct completion *x) { x->done = 0; }
static inline void reinit_completion(struct completion *x) { x->done = 0; }

static inline void complete(struct completion *x) {
	smp_mb();
	__atomic_fetch_add(&x->done, 1u, __ATOMIC_SEQ_CST);
}
static inline void complete_all(struct completion *x) {
	smp_mb();
	__atomic_store_n(&x->done, 0x7fffffffu, __ATOMIC_SEQ_CST);
}

static inline void wait_for_completion(struct completion *x) {
	void *ra = __builtin_return_address(0);
	while (__atomic_load_n(&x->done, __ATOMIC_ACQUIRE) == 0) {
		lkpi_wait_pump();
		lkpi_spin_probe(ra);
		__asm__ __volatile__("pause");
	}
	if (x->done != 0x7fffffffu)
		__atomic_fetch_sub(&x->done, 1u, __ATOMIC_SEQ_CST);
}

/* timeout variant: spins; returns remaining "jiffies" (>0) — never times out in bring-up. */
static inline unsigned long wait_for_completion_timeout(struct completion *x, unsigned long t) {
	wait_for_completion(x);
	return t ? t : 1;
}
static inline int wait_for_completion_interruptible(struct completion *x) {
	wait_for_completion(x);
	return 0;
}
static inline int try_wait_for_completion(struct completion *x) {
	if (__atomic_load_n(&x->done, __ATOMIC_ACQUIRE) == 0)
		return 0;
	if (x->done != 0x7fffffffu)
		__atomic_fetch_sub(&x->done, 1u, __ATOMIC_SEQ_CST);
	return 1;
}
static inline int completion_done(struct completion *x) {
	return __atomic_load_n(&x->done, __ATOMIC_ACQUIRE) != 0;
}

#endif /* _LINUXKPI_LINUX_COMPLETION_H */
