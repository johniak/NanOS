/* linuxkpi/include/linux/swap.h — i915 shrinker touches the LRU/unevictable helpers. NanOS has no
 * swap or page reclaim, so these are no-ops (the shrinker itself is registered-but-not-invoked). */
#ifndef _LINUXKPI_LINUX_SWAP_H
#define _LINUXKPI_LINUX_SWAP_H
#include <linux/types.h>
/* i915_gem_shrinker/i915_gem_shmem reach struct shrinker + writeback_control transitively through
 * <linux/swap.h> in mainline (not a direct include); mirror that so those files see the full types. */
#include <linux/shrinker.h>
#include <linux/writeback.h>
struct folio; struct list_head; struct pagevec;
#define SWAP_CLUSTER_MAX 32UL
static inline void mark_page_accessed(void *page){ (void)page; }
/* folio_mark_accessed + check_move_unevictable_folios live in the shim's pagemap.h */
#define total_swap_pages 0UL
#endif
