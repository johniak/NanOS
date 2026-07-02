#ifndef _LKPI_MUTEX_H
#define _LKPI_MUTEX_H
#include <linux/spinlock.h>
/* `held` tracks lock ownership truthfully for mutex_is_locked() queries. The acquire itself stays
 * a no-op (raw_spin_lock is a no-op on this UP cooperative kext — see spinlock.h: a real lock would
 * self-deadlock under the re-entrant vq pump / nested DRM modeset locking). Without `held`,
 * mutex_is_locked() always read 0, so the many WARN_ON(!mutex_is_locked(...)) / drm_modeset_is_locked
 * assertions in DRM atomic-commit + GEM paths fired on EVERY commit and flooded the graphical
 * console (fbcon), which under KMS re-grabbed the scanout and overwrote the compositor's frame. */
struct mutex { raw_spinlock_t l; int held; };
#define DEFINE_MUTEX(name) struct mutex name = { __RAW_SPIN_LOCK_INITIALIZER, 0 }
static inline void mutex_init(struct mutex *m){ raw_spin_lock_init(&m->l); m->held = 0; }
static inline void mutex_lock(struct mutex *m){ raw_spin_lock(&m->l); m->held = 1; }
static inline void mutex_unlock(struct mutex *m){ m->held = 0; raw_spin_unlock(&m->l); }
static inline int mutex_lock_interruptible(struct mutex *m){ raw_spin_lock(&m->l); m->held = 1; return 0; }
static inline int mutex_trylock(struct mutex *m){ if(!__lk_try(&m->l.lock)) return 0; m->held = 1; return 1; }
static inline int mutex_is_locked(struct mutex *m){ return m->held; }
#define mutex_destroy(m) do{}while(0)
#define might_lock(m) do{}while(0)
#endif

#ifndef _LKPI_MUTEX_DEC
#define _LKPI_MUTEX_DEC
#include <linux/atomic.h>
static inline int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock){ if(!atomic_dec_and_test(cnt)) return 0; mutex_lock(lock); return 1; }
#endif
