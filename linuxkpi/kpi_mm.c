/*
 * linuxkpi/kpi_mm.c — page-aligned allocations for the shim.
 *
 * NanOS RAM is identity-mapped (virt == phys), so page memory from the kernel heap is
 * directly DMA-addressable and virt_to_phys() (linux/mm.h) is the identity. We over-
 * allocate and align up, stashing the original heap pointer just below the aligned block
 * so free can recover it.
 */
#include <linux/mm.h>
#include <linux/string.h>
#include "lkpi_knx.h"

#define PG_ALIGN 4096UL

/*
 * mem_map: one struct page per physical page frame, so a struct page* can carry per-page metadata
 * (flags/refcount/private/lru) SEPARATE from the page's data — the real Linux model, required by
 * TTM and the rest of i915. Sized from knx_ram_top() (highest usable phys address) at kext load;
 * ~1.5% of RAM, like Linux. page_to_pfn(p) = p - lkpi_mem_map (see linux/mm.h).
 */
struct page *lkpi_mem_map = 0;
unsigned long lkpi_mem_map_pfns = 0;

int lkpi_mem_map_init(void) {
	if (lkpi_mem_map)
		return 1;   /* idempotent */
	unsigned long long top = knx_ram_top();
	unsigned long pfns = (unsigned long)(top >> PAGE_SHIFT) + 1;
	unsigned long bytes = pfns * sizeof(struct page);
	struct page *map = (struct page *)knx_malloc((unsigned)bytes);
	if (!map) {
		knx_log("lkpi: FATAL mem_map alloc failed\n");
		return 0;   /* leaves lkpi_mem_map NULL; caller MUST abort (a page access would fault) */
	}
	memset(map, 0, bytes);
	lkpi_mem_map_pfns = pfns;
	lkpi_mem_map = map;   /* publish last: any concurrent reader sees a fully-zeroed map */
	return 1;
}

void *alloc_pages_exact(size_t size, gfp_t gfp) {
	if (size == 0)
		size = 1;
	/* room to align up to a page boundary + a slot for the base pointer */
	unsigned long raw = (unsigned long)knx_malloc((unsigned)(size + PG_ALIGN + sizeof(void *)));
	if (!raw)
		return 0;
	unsigned long aligned = (raw + sizeof(void *) + (PG_ALIGN - 1)) & ~(PG_ALIGN - 1);
	((void **)aligned)[-1] = (void *)raw;
	if (gfp & __GFP_ZERO) {
		unsigned char *p = (unsigned char *)aligned;
		for (size_t i = 0; i < size; i++)
			p[i] = 0;
	}
	return (void *)aligned;
}

void free_pages_exact(void *virt, size_t size) {
	(void)size;
	if (!virt)
		return;
	void *base = ((void **)virt)[-1];
	/* Guard: a valid alloc_pages_exact block stashes its raw base in the word just below the
	 * aligned pointer, and that base is always < virt and non-NULL. If it isn't, `virt` is an
	 * INTERIOR address (e.g. a slice of a contiguous shmem block, whose only header sits at the
	 * base) — freeing it would hand knx_free() garbage and corrupt the heap. Skip + log loudly. */
	if (!base || (unsigned long)base >= (unsigned long)virt ||
	    (unsigned long)virt - (unsigned long)base > 4096UL + sizeof(void *)) {
		extern int printk(const char*,...);
		printk("lkpi: free_pages_exact SKIP interior/bad virt=%lx base=%lx\n",
		       (unsigned long)virt, (unsigned long)base);
		return;
	}
	knx_free(base);
}
