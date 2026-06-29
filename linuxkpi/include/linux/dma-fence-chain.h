#ifndef _LKPI_DMA_FENCE_CHAIN_H
#define _LKPI_DMA_FENCE_CHAIN_H
#include <linux/dma-fence.h>
struct dma_fence_chain { struct dma_fence base; struct dma_fence *prev; u64 prev_seqno; };
static inline struct dma_fence_chain *dma_fence_chain_alloc(void){ return 0; }
static inline void dma_fence_chain_free(struct dma_fence_chain *c){ (void)c; }
#endif

#ifndef _LKPI_FENCE_CHAIN_INIT
#define _LKPI_FENCE_CHAIN_INIT
static inline void dma_fence_chain_init(struct dma_fence_chain *c, struct dma_fence *p, struct dma_fence *f, u64 seq){ (void)c;(void)p;(void)f;(void)seq; }
#endif

#ifndef _LKPI_FENCE_CHAIN_X
#define _LKPI_FENCE_CHAIN_X
struct dma_fence_chain *to_dma_fence_chain(struct dma_fence *f);
int dma_fence_chain_find_seqno(struct dma_fence **pfence, uint64_t seqno);
struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence);
#define dma_fence_chain_for_each(iter, head) for (iter = dma_fence_get(head); iter; iter = dma_fence_chain_walk(iter))
#endif
