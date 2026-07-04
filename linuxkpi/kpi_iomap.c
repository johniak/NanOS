/*
 * linuxkpi/kpi_iomap.c — io_mapping over knx_map_mmio.
 *
 * Linux io_mapping is a short-lived WC kmap of an MMIO aperture (the i915 mappable GTT aperture,
 * BAR2 on Gen9). NanOS is identity-mapped with no highmem, so a "map" is just a pointer into the
 * aperture. We map the whole aperture ONCE, lazily on first use, and hand back base+offset for
 * every map call; the unmap calls are no-ops (the aperture stays mapped for the io_mapping's life).
 *
 * Write-combining requires PAT/MTRR control, which the NanOS kernel does not expose (no PAT/MTRR
 * code), so the mapping is uncached — writes still land, they are just not coalesced. That is a
 * correctness-preserving perf follow-on; a one-time notice records it.
 *
 * NOTE (Phase B / Dell): knx_map_mmio's ABI is 32-bit (phys, len). If a real i915 aperture BAR
 * sits above 4 GiB, the base truncates here — widening the knx MMIO ABI is tracked separately
 * (the same 32-bit limit the net stack lives with). QEMU never exercises this (no i915 device).
 */
#include <linux/io-mapping.h>
#include "lkpi_knx.h"

static int g_uc_noticed;

/* Map the whole aperture on first use, caching it in the io_mapping. */
static void __iomem *iomap_base(struct io_mapping *m)
{
	if (!m) return 0;
	if (!m->iomem) {
		if (!g_uc_noticed) {
			knx_log("lkpi: io_mapping UC (no PAT)\n");
			g_uc_noticed = 1;
		}
		m->iomem = knx_map_mmio((unsigned) m->base, (unsigned) m->size);
	}
	return m->iomem;
}

struct io_mapping *io_mapping_init_wc(struct io_mapping *iomap, resource_size_t base,
		unsigned long size)
{
	if (!iomap) return 0;
	iomap->base = base;
	iomap->size = size;
	iomap->iomem = 0;
	return iomap;
}

void io_mapping_fini(struct io_mapping *mapping)
{
	/* No MMIO unmap facility; the aperture mapping persists (documented). Clear the cache so a
	 * re-init re-maps. */
	if (mapping) mapping->iomem = 0;
}

struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size)
{
	struct io_mapping *m = (struct io_mapping *) knx_malloc(sizeof *m);
	if (!m) return 0;
	return io_mapping_init_wc(m, base, size);
}

void io_mapping_free(struct io_mapping *iomap)
{
	if (iomap) knx_free(iomap);
}

void __iomem *io_mapping_map_wc(struct io_mapping *mapping, unsigned long offset, unsigned long size)
{
	void __iomem *b = iomap_base(mapping);
	(void) size;
	return b ? (void __iomem *) ((char *) b + offset) : 0;
}

void __iomem *io_mapping_map_atomic_wc(struct io_mapping *mapping, unsigned long offset)
{
	void __iomem *b = iomap_base(mapping);
	return b ? (void __iomem *) ((char *) b + offset) : 0;
}

void __iomem *io_mapping_map_local_wc(struct io_mapping *mapping, unsigned long offset)
{
	void __iomem *b = iomap_base(mapping);
	return b ? (void __iomem *) ((char *) b + offset) : 0;
}

void io_mapping_unmap(void __iomem *vaddr)        { (void) vaddr; }
void io_mapping_unmap_atomic(void __iomem *vaddr) { (void) vaddr; }
void io_mapping_unmap_local(void __iomem *vaddr)  { (void) vaddr; }
