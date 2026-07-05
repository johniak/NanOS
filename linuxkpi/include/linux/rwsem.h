/*
 * linuxkpi/include/linux/rwsem.h — read/write semaphore.
 *
 * Backed by the shim mutex: both down_read and down_write take it exclusively. This
 * over-serializes concurrent readers (correctness over throughput) which is fine for the
 * bring-up paths that use it (TTM pool shrink rwsem, a few i915 registration rwsems). A
 * true reader/writer lock is a measured follow-on if a read-heavy path shows contention.
 */
#ifndef _LKPI_RWSEM_H
#define _LKPI_RWSEM_H
#include <linux/mutex.h>

struct rw_semaphore { struct mutex m; };

#define DECLARE_RWSEM(name) struct rw_semaphore name = { .m = { __RAW_SPIN_LOCK_INITIALIZER, 0 } }
#define __RWSEM_INITIALIZER(name) { .m = { __RAW_SPIN_LOCK_INITIALIZER, 0 } }

static inline void init_rwsem(struct rw_semaphore *s){ mutex_init(&s->m); }
static inline void down_read(struct rw_semaphore *s){ mutex_lock(&s->m); }
static inline int  down_read_interruptible(struct rw_semaphore *s){ return mutex_lock_interruptible(&s->m); }
static inline int  down_read_trylock(struct rw_semaphore *s){ return mutex_trylock(&s->m); }
static inline void up_read(struct rw_semaphore *s){ mutex_unlock(&s->m); }
static inline void down_write(struct rw_semaphore *s){ mutex_lock(&s->m); }
static inline int  down_write_killable(struct rw_semaphore *s){ mutex_lock(&s->m); return 0; }
static inline int  down_write_trylock(struct rw_semaphore *s){ return mutex_trylock(&s->m); }
static inline void up_write(struct rw_semaphore *s){ mutex_unlock(&s->m); }
static inline void downgrade_write(struct rw_semaphore *s){ (void)s; }
static inline int  rwsem_is_locked(struct rw_semaphore *s){ return s->m.held; }

#endif
