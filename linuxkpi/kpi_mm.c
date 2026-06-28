/*
 * linuxkpi/kpi_mm.c — page-aligned allocations for the shim.
 *
 * NanOS RAM is identity-mapped (virt == phys), so page memory from the kernel heap is
 * directly DMA-addressable and virt_to_phys() (linux/mm.h) is the identity. We over-
 * allocate and align up, stashing the original heap pointer just below the aligned block
 * so free can recover it.
 */
#include <linux/mm.h>
#include "lkpi_knx.h"

#define PG_ALIGN 4096UL

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
	knx_free(((void **)virt)[-1]);
}
