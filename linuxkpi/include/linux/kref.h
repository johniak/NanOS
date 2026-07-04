/* linuxkpi/include/linux/kref.h — reference counts for the shim (atomic). */
#ifndef _LINUXKPI_LINUX_KREF_H
#define _LINUXKPI_LINUX_KREF_H

#include <linux/atomic.h>

struct kref { atomic_t refcount; };

static inline void kref_init(struct kref *kref) { atomic_set(&kref->refcount, 1); }
static inline void kref_get(struct kref *kref) { atomic_inc(&kref->refcount); }
static inline unsigned int kref_read(const struct kref *kref) { return atomic_read(&kref->refcount); }
static inline int kref_put(struct kref *kref, void (*release)(struct kref *kref)) {
	if (atomic_dec_and_test(&kref->refcount)) { release(kref); return 1; }
	return 0;
}
static inline int kref_get_unless_zero(struct kref *kref) {
	int c = atomic_read(&kref->refcount);
	while (c) { if (atomic_cmpxchg(&kref->refcount, c, c + 1) == c) return 1; c = atomic_read(&kref->refcount); }
	return 0;
}
#include <linux/mutex.h>
/* kref_put_lock: drop the ref, and if it hit zero take `lock` before calling release (which unlocks).
 * The shim's mutex isn't reentrant-sensitive here; take it on the zero transition as Linux does. */
static inline int kref_put_lock(struct kref *kref, void (*release)(struct kref *kref), struct mutex *lock) {
	if (atomic_dec_and_test(&kref->refcount)) { mutex_lock(lock); release(kref); return 1; }
	return 0;
}

#endif
