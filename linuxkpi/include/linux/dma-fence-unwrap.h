#ifndef _LKPI_DMA_FENCE_UNWRAP_H
#define _LKPI_DMA_FENCE_UNWRAP_H
#include <linux/dma-fence.h>
struct dma_fence_unwrap { int n; };
#define dma_fence_unwrap_for_each(f,it,head) for(f=(head);f;f=0)
static inline struct dma_fence *dma_fence_unwrap_merge(struct dma_fence *f, ...){ return f; }
#endif
