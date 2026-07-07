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
/* Upstream semantics (dma-fence-array.c): a PLAIN fence — not a dma_fence_array — iterates
 * exactly ONCE, as itself; NULL is only for a NULL head or an empty array. The shim never
 * constructs arrays (dma_fence_array_create returns NULL), so first() IS the head and next()
 * ends the walk. The old always-NULL stubs made dma_fence_array_for_each iterate ZERO times,
 * which silently skipped the dma_resv_add_fence loop in _i915_vma_move_to_active
 * (i915_vma.c:1998) — no execbuf fence ever landed on a BO's resv, so GEM_WAIT/GEM_BUSY saw
 * empty reservations and EVERY request looked complete at birth (Dell boots #33-#38: false
 * 'store OK' races, a spin batch 'completing' in 1 ms while physically spinning the engine,
 * guilty=0 forever). Shim-broke-an-invariant bug class, instance #6. */
static inline struct dma_fence *dma_fence_array_first(struct dma_fence *head){ return head; }
static inline struct dma_fence *dma_fence_array_next(struct dma_fence *head, unsigned int index){ (void)head;(void)index; return 0; }
/* iterate the fences of an array (or the single fence if not an array). */
#define dma_fence_array_for_each(fence, index, head) \
	for (index = 0, fence = dma_fence_array_first(head); fence; fence = dma_fence_array_next(head, ++(index)))
#endif
