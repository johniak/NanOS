#ifndef _LKPI_WW_MUTEX_H
#define _LKPI_WW_MUTEX_H
#include <linux/mutex.h>
struct ww_class { int n; };
struct ww_acquire_ctx { int n; };
struct ww_mutex { struct mutex base; };
#define DEFINE_WW_CLASS(name) struct ww_class name = {0}
static inline void ww_mutex_init(struct ww_mutex *l, struct ww_class *c){ (void)c; mutex_init(&l->base); }
static inline void ww_acquire_init(struct ww_acquire_ctx *c, struct ww_class *cl){ (void)c;(void)cl; }
static inline void ww_acquire_fini(struct ww_acquire_ctx *c){ (void)c; }
static inline void ww_acquire_done(struct ww_acquire_ctx *c){ (void)c; }
static inline int  ww_mutex_lock(struct ww_mutex *l, struct ww_acquire_ctx *c){ (void)c; mutex_lock(&l->base); return 0; }
static inline int  ww_mutex_lock_interruptible(struct ww_mutex *l, struct ww_acquire_ctx *c){ (void)c; mutex_lock(&l->base); return 0; }
static inline int  ww_mutex_trylock(struct ww_mutex *l, struct ww_acquire_ctx *c){ (void)c; return mutex_trylock(&l->base); }
static inline void ww_mutex_unlock(struct ww_mutex *l){ mutex_unlock(&l->base); }
static inline int  ww_mutex_is_locked(struct ww_mutex *l){ return mutex_is_locked(&l->base); }
#endif

#ifndef _LKPI_WW_RESV
#define _LKPI_WW_RESV
extern struct ww_class reservation_ww_class;
#endif
