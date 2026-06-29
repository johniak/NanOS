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
#ifdef __cplusplus
}
#endif
#define dma_resv_held(r) (1)
#define dma_resv_assert_held(r) do{}while(0)
#endif

#ifndef _LKPI_DMA_RESV_EXTRA
#define _LKPI_DMA_RESV_EXTRA
static inline bool dma_resv_test_signaled(struct dma_resv *r, enum dma_resv_usage u){ (void)r;(void)u; return true; }
#endif
