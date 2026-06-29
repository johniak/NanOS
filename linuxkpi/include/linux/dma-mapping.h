/*
 * linuxkpi/include/linux/dma-mapping.h — DMA API for the shim.
 *
 * NanOS is identity-mapped with no IOMMU, so dma_addr == phys == virt: the map/unmap calls
 * are no-ops returning the address, and coherent allocations come from knx_dma_alloc
 * (contiguous, physically addressable). Implemented in linuxkpi/kpi_dma.c.
 */
#ifndef _LINUXKPI_LINUX_DMA_MAPPING_H
#define _LINUXKPI_LINUX_DMA_MAPPING_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/scatterlist.h>
#include <linux/dma-direction.h>

#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR (~(dma_addr_t)0)

#ifdef __cplusplus
extern "C" {
#endif

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp);
void  dma_free_coherent(struct device *dev, size_t size, void *vaddr, dma_addr_t dma_handle);
dma_addr_t dma_map_single_attrs(struct device *dev, void *ptr, size_t size,
                                enum dma_data_direction dir, unsigned long attrs);
void dma_unmap_single_attrs(struct device *dev, dma_addr_t addr, size_t size,
                            enum dma_data_direction dir, unsigned long attrs);

#ifdef __cplusplus
}
#endif

#define dma_map_single(d, p, s, dir)   dma_map_single_attrs(d, p, s, dir, 0)
#define dma_unmap_single(d, a, s, dir) dma_unmap_single_attrs(d, a, s, dir, 0)

static inline int dma_mapping_error(struct device *dev, dma_addr_t addr) {
	(void)dev; return addr == DMA_MAPPING_ERROR;
}
static inline int dma_set_mask(struct device *dev, u64 mask) { (void)dev; (void)mask; return 0; }
static inline int dma_set_coherent_mask(struct device *dev, u64 mask) { (void)dev; (void)mask; return 0; }
static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask) { (void)dev; (void)mask; return 0; }
static inline u64 dma_get_required_mask(struct device *dev) { (void)dev; return DMA_BIT_MASK(64); }
static inline size_t dma_max_mapping_size(struct device *dev) { (void)dev; return (size_t)-1; }
static inline int dma_set_max_seg_size(struct device *dev, unsigned int size) { (void)dev; (void)size; return 0; }
static inline void dma_sync_single_for_cpu(struct device *d, dma_addr_t a, size_t s, enum dma_data_direction dir) { (void)d;(void)a;(void)s;(void)dir; }
static inline void dma_sync_single_for_device(struct device *d, dma_addr_t a, size_t s, enum dma_data_direction dir) { (void)d;(void)a;(void)s;(void)dir; }
static inline void dma_sync_single_range_for_cpu(struct device *d, dma_addr_t a, unsigned long off, size_t s, enum dma_data_direction dir) { (void)d;(void)a;(void)off;(void)s;(void)dir; }
static inline void dma_sync_single_range_for_device(struct device *d, dma_addr_t a, unsigned long off, size_t s, enum dma_data_direction dir) { (void)d;(void)a;(void)off;(void)s;(void)dir; }
static inline bool dma_need_sync(struct device *dev, dma_addr_t addr) { (void)dev;(void)addr; return false; }

/* page maps: phys == virt under identity mapping */
static inline dma_addr_t dma_map_page_attrs(struct device *dev, void *page, size_t off,
                                            size_t size, enum dma_data_direction dir, unsigned long attrs) {
	(void)dev;(void)size;(void)dir;(void)attrs; return (dma_addr_t)((unsigned long)page + off);
}
#define dma_map_page(d, pg, off, sz, dir) dma_map_page_attrs(d, pg, off, sz, dir, 0)
static inline void dma_unmap_page_attrs(struct device *d, dma_addr_t a, size_t s, enum dma_data_direction dir, unsigned long attrs) { (void)d;(void)a;(void)s;(void)dir;(void)attrs; }
#define dma_unmap_page(d, a, sz, dir) dma_unmap_page_attrs(d, a, sz, dir, 0)

#endif /* _LINUXKPI_LINUX_DMA_MAPPING_H */
