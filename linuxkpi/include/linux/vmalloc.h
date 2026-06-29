#ifndef _LKPI_VMALLOC_H
#define _LKPI_VMALLOC_H
#include <linux/mm.h>
#define PAGE_KERNEL 0
#define PAGE_KERNEL_IO 0
#define VM_MAP 0x00000004
static inline void *vmap(struct page **pages, unsigned count, unsigned long flags, pgprot_t prot){ (void)flags;(void)prot; return count?page_address(pages[0]):0; }
static inline void vunmap(const void *addr){ (void)addr; }
static inline void *vmap_pfn(unsigned long *pfns, unsigned count, pgprot_t prot){ (void)pfns;(void)count;(void)prot; return 0; }
#endif
