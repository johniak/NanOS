/*
 * linuxkpi/include/linux/refcount.h — refcount_t. The type and the core ops (set/read/inc/
 * dec_and_test/inc_not_zero) already live in the shim's atomic.h; this forwards there and adds the
 * few extra ops i915 references. (Saturating overflow semantics are not enforced — NanOS bring-up
 * does not stress the guards.)
 */
#ifndef _LINUXKPI_LINUX_REFCOUNT_H
#define _LINUXKPI_LINUX_REFCOUNT_H

#include <linux/atomic.h>   /* refcount_t (field .r) + set/read/inc/dec_and_test/inc_not_zero */

#ifndef REFCOUNT_INIT
#define REFCOUNT_INIT(n) { .r = ATOMIC_INIT(n) }
#endif

static inline void refcount_dec(refcount_t *r)                 { atomic_dec(&r->r); }
static inline void refcount_add(int i, refcount_t *r)          { atomic_add(i, &r->r); }
static inline bool refcount_sub_and_test(int i, refcount_t *r) { return atomic_sub_and_test(i, &r->r); }
static inline bool refcount_add_not_zero(int i, refcount_t *r)
{
	if (!atomic_read(&r->r)) return false;
	atomic_add(i, &r->r);
	return true;
}

#endif /* _LINUXKPI_LINUX_REFCOUNT_H */
