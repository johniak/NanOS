#ifndef _LKPI_DMA_RESV_H
#define _LKPI_DMA_RESV_H
#include <linux/types.h>
#include <linux/ww_mutex.h>
#include <linux/dma-fence.h>
struct dma_resv { struct ww_mutex lock; void *fences; };
enum dma_resv_usage { DMA_RESV_USAGE_KERNEL=0, DMA_RESV_USAGE_WRITE, DMA_RESV_USAGE_READ, DMA_RESV_USAGE_BOOKKEEP };
#ifdef __cplusplus
extern "C" {
#endif
void dma_resv_init(struct dma_resv*);
void dma_resv_fini(struct dma_resv*);
int  dma_resv_lock(struct dma_resv*, struct ww_acquire_ctx*);
int  dma_resv_lock_interruptible(struct dma_resv*, struct ww_acquire_ctx*);
bool dma_resv_trylock(struct dma_resv*);
void dma_resv_unlock(struct dma_resv*);
int  dma_resv_reserve_fences(struct dma_resv*, unsigned);
void dma_resv_add_fence(struct dma_resv*, struct dma_fence*, enum dma_resv_usage);
long dma_resv_wait_timeout(struct dma_resv*, enum dma_resv_usage, bool, long);
bool dma_resv_test_signaled(struct dma_resv*, enum dma_resv_usage);
#ifdef __cplusplus
}
#endif
#define dma_resv_held(r) (1)
#define dma_resv_assert_held(r) do{}while(0)
#endif

/* dma_resv_test_signaled is a REAL function (kpi_fence.c). It was once an always-true inline
 * stub here — that lie made VIRTGPU_WAIT(NOWAIT) report every buffer idle, so Mesa's virgl
 * winsys recycled transfer-staging buffers while the host was still consuming them (the GL
 * desktop's cross-window texture shred under changing content). Busy checks must consult the
 * tracked fence. */

#ifndef _LKPI_DMA_RESV_USAGE
#define _LKPI_DMA_RESV_USAGE
static inline enum dma_resv_usage dma_resv_usage_rw(bool write){ return write?DMA_RESV_USAGE_WRITE:DMA_RESV_USAGE_READ; }
#endif

#ifndef _LKPI_DMA_RESV_SLOW
#define _LKPI_DMA_RESV_SLOW
static inline int dma_resv_lock_slow_interruptible(struct dma_resv *r, struct ww_acquire_ctx *c){ return dma_resv_lock_interruptible(r,c); }
static inline void dma_resv_lock_slow(struct dma_resv *r, struct ww_acquire_ctx *c){ dma_resv_lock(r,c); }
#endif

#ifndef _LKPI_DMA_RESV_SINGLETON
#define _LKPI_DMA_RESV_SINGLETON
/* Return the tracked fence (referenced), not an unconditional NULL: prepare_fb/plane commit
 * uses this to wait for a buffer's producer before scanning it out. */
static inline int dma_resv_get_singleton(struct dma_resv *r, enum dma_resv_usage u, struct dma_fence **f){ (void)u; *f = (r && r->fences) ? dma_fence_get((struct dma_fence *)r->fences) : 0; return 0; }
#endif
