/*
 * linuxkpi/include/linux/io-mapping.h — io_mapping over knx_map_mmio (kpi_iomap.c).
 *
 * Linux io_mapping abstracts short-lived WC kmaps of an MMIO aperture (e.g. the i915 GTT
 * mappable aperture). NanOS is identity-mapped with no highmem, so a mapping is just a pointer
 * into the aperture: we map the whole aperture once (lazily, on first use) via knx_map_mmio and
 * hand back base+offset. Write-combining needs PAT/MTRR control, which the NanOS kernel does not
 * expose — so the mapping is uncached (UC) with a one-time notice. Correctness first; WC is a
 * documented perf follow-on (writes still land, just not coalesced).
 */
#ifndef _LINUXKPI_LINUX_IO_MAPPING_H
#define _LINUXKPI_LINUX_IO_MAPPING_H

#include <linux/types.h>
#include <linux/bug.h>

#ifdef __cplusplus
extern "C" {
#endif

struct io_mapping {
	resource_size_t base;      /* physical base of the aperture */
	unsigned long   size;      /* aperture length in bytes */
	void __iomem   *iomem;     /* cached whole-aperture mapping (NULL until first map) */
};

struct io_mapping *io_mapping_init_wc(struct io_mapping *iomap, resource_size_t base,
		unsigned long size);
void io_mapping_fini(struct io_mapping *mapping);

struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size);
void io_mapping_free(struct io_mapping *iomap);

/* Map a window at `offset` within the aperture. `size` is advisory here (the whole aperture is
 * mapped); atomic/local variants map a single page (offset only). All return an __iomem pointer;
 * the unmap variants are no-ops (the aperture mapping persists for the io_mapping's lifetime). */
void __iomem *io_mapping_map_wc(struct io_mapping *mapping, unsigned long offset, unsigned long size);
void __iomem *io_mapping_map_atomic_wc(struct io_mapping *mapping, unsigned long offset);
void __iomem *io_mapping_map_local_wc(struct io_mapping *mapping, unsigned long offset);
void io_mapping_unmap(void __iomem *vaddr);
void io_mapping_unmap_atomic(void __iomem *vaddr);
void io_mapping_unmap_local(void __iomem *vaddr);

#ifdef __cplusplus
}
#endif

#endif /* _LINUXKPI_LINUX_IO_MAPPING_H */
