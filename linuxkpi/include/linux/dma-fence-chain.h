#ifndef _LKPI_DMA_FENCE_CHAIN_H
#define _LKPI_DMA_FENCE_CHAIN_H
#include <linux/dma-fence.h>
struct dma_fence_chain { struct dma_fence base; };
static inline struct dma_fence_chain *dma_fence_chain_alloc(void){ return 0; }
static inline void dma_fence_chain_free(struct dma_fence_chain *c){ (void)c; }
#endif
