/*
 * linuxkpi/kpi_dma.c — DMA API for the shim.
 *
 * NanOS is identity-mapped with no IOMMU, so the DMA address of any kernel buffer is its
 * virtual address. dma_alloc_coherent returns page-aligned heap memory (see kpi_mm.c) and
 * reports dma_handle == virt; map/unmap are pass-throughs.
 */
#include <linux/dma-mapping.h>
#include <linux/mm.h>

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp) {
	(void)dev;
	void *v = alloc_pages_exact(size, gfp | __GFP_ZERO);
	if (!v) {
		if (dma_handle) *dma_handle = 0;
		return 0;
	}
	if (dma_handle)
		*dma_handle = (dma_addr_t)(unsigned long)v;   /* identity map: virt == phys */
	return v;
}

void dma_free_coherent(struct device *dev, size_t size, void *vaddr, dma_addr_t dma_handle) {
	(void)dev; (void)dma_handle;
	free_pages_exact(vaddr, size);
}

dma_addr_t dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
                                enum dma_data_direction dir, unsigned long attrs) {
	(void)dev; (void)size; (void)dir; (void)attrs;
	return (dma_addr_t)(unsigned long)ptr;   /* identity map */
}

void dma_unmap_single_attrs(struct device *dev, dma_addr_t addr, size_t size,
                            enum dma_data_direction dir, unsigned long attrs) {
	(void)dev; (void)addr; (void)size; (void)dir; (void)attrs;
}
