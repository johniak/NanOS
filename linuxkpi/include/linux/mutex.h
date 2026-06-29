#ifndef _LKPI_MUTEX_H
#define _LKPI_MUTEX_H
#include <linux/spinlock.h>
struct mutex { raw_spinlock_t l; };
#define DEFINE_MUTEX(name) struct mutex name = { __RAW_SPIN_LOCK_INITIALIZER }
static inline void mutex_init(struct mutex *m){ raw_spin_lock_init(&m->l); }
static inline void mutex_lock(struct mutex *m){ raw_spin_lock(&m->l); }
static inline void mutex_unlock(struct mutex *m){ raw_spin_unlock(&m->l); }
static inline int mutex_lock_interruptible(struct mutex *m){ raw_spin_lock(&m->l); return 0; }
static inline int mutex_trylock(struct mutex *m){ return __lk_try(&m->l.lock); }
static inline int mutex_is_locked(struct mutex *m){ return m->l.lock; }
#define mutex_destroy(m) do{}while(0)
#define might_lock(m) do{}while(0)
#endif
