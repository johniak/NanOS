#ifndef _LKPI_WW_MUTEX_H
#define _LKPI_WW_MUTEX_H
#include <linux/mutex.h>
#include <linux/errno.h>
struct ww_class { int n; };
struct ww_acquire_ctx { int n; };
/* `owner` records the acquire-ctx currently holding the lock. Real ww_mutex returns -EALREADY when
 * a ctx re-locks a lock it already holds; callers (drm_modeset_lock) rely on that to AVOID a second
 * list_add of the lock onto ctx->locked. Without it the same node is added twice, ctx->locked goes
 * circular, and drm_modeset_drop_locks() spins forever (the recursive-lock path is hit by
 * drm_helper_probe_single_connector_modes locking connection_mutex, then drm_helper_probe_detect
 * locking it again). The underlying mutex is a UP no-op spinlock, so this owner field IS the whole
 * recursion check — mirror real ww_mutex semantics with it. */
struct ww_mutex { struct mutex base; struct ww_acquire_ctx *owner; };
#define DEFINE_WW_CLASS(name) struct ww_class name = {0}
static inline void ww_mutex_init(struct ww_mutex *l, struct ww_class *c){ (void)c; mutex_init(&l->base); l->owner = 0; }
static inline void ww_acquire_init(struct ww_acquire_ctx *c, struct ww_class *cl){ (void)c;(void)cl; }
static inline void ww_acquire_fini(struct ww_acquire_ctx *c){ (void)c; }
static inline void ww_acquire_done(struct ww_acquire_ctx *c){ (void)c; }
static inline int  ww_mutex_lock(struct ww_mutex *l, struct ww_acquire_ctx *c){ if (c && l->owner == c) return -EALREADY; mutex_lock(&l->base); l->owner = c; return 0; }
static inline int  ww_mutex_lock_interruptible(struct ww_mutex *l, struct ww_acquire_ctx *c){ if (c && l->owner == c) return -EALREADY; mutex_lock(&l->base); l->owner = c; return 0; }
static inline int  ww_mutex_trylock(struct ww_mutex *l, struct ww_acquire_ctx *c){ if (!mutex_trylock(&l->base)) return 0; l->owner = c; return 1; }
static inline void ww_mutex_unlock(struct ww_mutex *l){ l->owner = 0; mutex_unlock(&l->base); }
static inline int  ww_mutex_is_locked(struct ww_mutex *l){ return mutex_is_locked(&l->base); }
#endif

#ifndef _LKPI_WW_RESV
#define _LKPI_WW_RESV
extern struct ww_class reservation_ww_class;
#endif

#ifndef _LKPI_WW_SLOW
#define _LKPI_WW_SLOW
static inline void ww_mutex_lock_slow(struct ww_mutex *l, struct ww_acquire_ctx *c){ ww_mutex_lock(l,c); }
static inline int ww_mutex_lock_slow_interruptible(struct ww_mutex *l, struct ww_acquire_ctx *c){ return ww_mutex_lock_interruptible(l,c); }
#endif
