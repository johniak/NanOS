/* linuxkpi/include/linux/dma-fence-array.h — fence-array (i915 merges in/out fences on submit).
 * Minimal: the merge path degrades to the single-fence path when array creation returns NULL.
 * FOLLOW-ON: wire the vendored dma-fence-array.c if multi-fence submit is exercised. */
#ifndef _LINUXKPI_LINUX_DMA_FENCE_ARRAY_H
#define _LINUXKPI_LINUX_DMA_FENCE_ARRAY_H
#include <linux/dma-fence.h>
struct dma_fence_array { struct dma_fence base; unsigned num_fences; struct dma_fence **fences; };
static inline struct dma_fence_array *to_dma_fence_array(struct dma_fence *f){ (void)f; return 0; }
static inline bool dma_fence_is_array(struct dma_fence *f){ (void)f; return false; }
static inline struct dma_fence_array *dma_fence_array_create(int n, struct dma_fence **fences,
		u64 context, unsigned seqno, bool signal_on_any){ (void)n;(void)fences;(void)context;(void)seqno;(void)signal_on_any; return 0; }
static inline struct dma_fence *dma_fence_array_first(struct dma_fence *head){ (void)head; return 0; }
static inline struct dma_fence *dma_fence_array_next(struct dma_fence *head, unsigned int index){ (void)head;(void)index; return 0; }
/* iterate the fences of an array (or the single fence if not an array). Empty in the shim. */
#define dma_fence_array_for_each(fence, index, head) \
	for (index = 0, fence = dma_fence_array_first(head); fence; fence = dma_fence_array_next(head, ++(index)))
#endif
