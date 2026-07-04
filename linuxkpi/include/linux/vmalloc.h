#ifndef _LKPI_VMALLOC_H
#define _LKPI_VMALLOC_H
#include <linux/mm.h>
#define PAGE_KERNEL 0
#define PAGE_KERNEL_IO 0
#define VM_MAP 0x00000004
static inline void *vmap(struct page **pages, unsigned count, unsigned long flags, pgprot_t prot){ (void)flags;(void)prot; return count?page_address(pages[0]):0; }
static inline void vunmap(const void *addr){ (void)addr; }
static inline void *vmap_pfn(unsigned long *pfns, unsigned count, pgprot_t prot){ (void)pfns;(void)count;(void)prot; return 0; }
struct notifier_block;
/* vmap-area purge notifier: i915 shrinker hooks it to drop cached vmaps under pressure. No vmalloc
 * reclaim in the shim, so registration is inert. */
static inline int register_vmap_purge_notifier(struct notifier_block *nb){ (void)nb; return 0; }
static inline int unregister_vmap_purge_notifier(struct notifier_block *nb){ (void)nb; return 0; }
#ifndef VM_MAP_PUT_PAGES
#define VM_MAP        0x00000004
#define VM_MAP_PUT_PAGES 0x00000200
#define VM_IOREMAP    0x00000001
#define VM_ALLOC      0x00000002
#endif
#endif
