#ifndef _LKPI_VMALLOC_H
#define _LKPI_VMALLOC_H
#include <linux/mm.h>
#include <linux/printk.h>
#define PAGE_KERNEL 0
#define PAGE_KERNEL_IO 0
#define VM_MAP 0x00000004
/* The shim has no scatter-gather vmap: it returns the linear kernel address of page[0] and relies on
 * page[1..n] being physically contiguous right after it. gem_shmem objects ARE one contiguous block
 * (kpi_misc.c shmem_file_setup), and i915's LRC context images happen to be single 2^order chunks —
 * but i915's internal-object allocator has a fallback path (i915_gem_internal.c: shrink the order on
 * alloc failure) that can return a MULTI-CHUNK page array. Mapping only page[0] would then silently
 * scribble past the first chunk into unrelated memory. Verify contiguity; refuse (return NULL, which
 * i915 handles as -ENOMEM) rather than corrupt. */
static inline void *vmap(struct page **pages, unsigned count, unsigned long flags, pgprot_t prot){
	unsigned i;
	(void)flags;(void)prot;
	if (!count) return 0;
	for (i = 1; i < count; i++) {
		if (page_to_phys(pages[i]) != page_to_phys(pages[0]) + (unsigned long)i * PAGE_SIZE) {
			static int once;
			if (!once) { once = 1; printk("lkpi: vmap NON-CONTIGUOUS (%u pages) — refusing (NULL)\n", count); }
			return 0;
		}
	}
	return page_address(pages[0]);
}
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
