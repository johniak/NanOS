/* linuxkpi/include/linux/swap.h — i915 shrinker touches the LRU/unevictable helpers. NanOS has no
 * swap or page reclaim, so these are no-ops (the shrinker itself is registered-but-not-invoked). */
#ifndef _LINUXKPI_LINUX_SWAP_H
#define _LINUXKPI_LINUX_SWAP_H
#include <linux/types.h>
struct folio; struct list_head; struct pagevec;
static inline void mark_page_accessed(void *page){ (void)page; }
/* folio_mark_accessed + check_move_unevictable_folios live in the shim's pagemap.h */
#define total_swap_pages 0UL
#endif
