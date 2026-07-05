/*
 * i915_entry.c — NanOS kext bootstrap that links and loads the UNMODIFIED Linux 6.12 i915
 * DRM driver against the LinuxKPI shim (Dell GPU plan, Task 5 final step).
 *
 * Mirrors kext/virtio_gpu/virtio_gpu_drv_entry.c, but i915 is a PCI driver (pci_register_driver
 * via i915_pci_register_driver) rather than a virtio driver. This entry:
 *   1. builds the LinuxKPI mem_map (one struct page per RAM frame) — every alloc_pages /
 *      virt_to_page / page_address in the driver + TTM indexes it,
 *   2. initializes the DRM core (chrdev/class + drm_core_init_complete),
 *   3. runs i915_init() (module_init) UNMODIFIED — registers the pci_driver and all the
 *      i915_*_module_init subfuncs (slab caches, schedulers, active tracking),
 *   4. matches the driver's own PCI id_table against the devices present via knx_pci_find, and
 *      reports the result.
 *
 * On QEMU (no Intel GPU) step 4 finds nothing and the module stays present-but-idle — the
 * intended state for this LINK milestone. Building a real pci_dev (BAR mapping, MSI, execlists)
 * and calling i915's probe() is Phase B (Dell Latitude 5310 bring-up, plan Tasks 6-8), where an
 * actual Comet Lake-U GT2 (8086:9B41) exists to drive and test against.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <drm/intel/intel-gtt.h>
#include "lkpi_knx.h"

/* ---- arch-provided globals the driver expects (x86 GPU stolen-memory resource) ------------ *
 * On a real x86 kernel this is populated by early PCI quirks (arch/x86/kernel/early-quirks.c)
 * from the host bridge's stolen-memory (BDSM) register. Our port has no early-quirk pass, so it
 * is defined empty here; i915's stolen setup treats a zero region as "no stolen memory".
 * PHASE B (Dell): populate .start/.end from the real BDSM base before enabling stolen-backed BOs. */
struct resource intel_graphics_stolen_res = { 0 };

/* ---- legacy GMCH GTT backend (intel-gtt.ko) — DEAD on Gen9.5 ------------------------------- *
 * i915's intel_ggtt_gmch.c calls these, but only on the GRAPHICS_VER < 6 path (gen2-5). Comet
 * Lake-U is VER 9 and uses gen8_gmch_probe, so none of these run. They stand in for the separate
 * CONFIG_INTEL_GTT module we do not build — the honest config-equivalent, not a stubbed protocol.
 * Each returns failure/no-op so that, were the legacy path ever taken, it fails cleanly. */
void intel_gmch_gtt_get(u64 *gtt_total, phys_addr_t *mappable_base, resource_size_t *mappable_end)
{ if (gtt_total) *gtt_total = 0; if (mappable_base) *mappable_base = 0; if (mappable_end) *mappable_end = 0; }
int intel_gmch_probe(struct pci_dev *bridge_pdev, struct pci_dev *gpu_pdev, struct agp_bridge_data *bridge)
{ (void)bridge_pdev; (void)gpu_pdev; (void)bridge; return -ENODEV; }
void intel_gmch_remove(void) {}
bool intel_gmch_enable_gtt(void) { return false; }
void intel_gmch_gtt_flush(void) {}
void intel_gmch_gtt_insert_page(dma_addr_t addr, unsigned int pg, unsigned int flags)
{ (void)addr; (void)pg; (void)flags; }
void intel_gmch_gtt_insert_sg_entries(struct sg_table *st, unsigned int pg_start, unsigned int flags)
{ (void)st; (void)pg_start; (void)flags; }
void intel_gmch_gtt_clear_range(unsigned int first_entry, unsigned int num_entries)
{ (void)first_entry; (void)num_entries; }

/* i915_init() (module_init) emitted as this global by the shim module_init macro. */
extern int __lkpi_modinit_i915_init(void);
/* DRM core's module_init(drm_core_init); must run before any drm_dev_register(). */
extern int __lkpi_modinit_drm_core_init(void);

/* Intel vendor id; Comet Lake-U GT2 (Dell Latitude 5310) is 8086:9B41. */
#define PCI_VENDOR_INTEL 0x8086

/* Scan the devices present against the driver's own PCI id_table (as the PCI bus match would),
 * so the report reflects exactly what i915 would bind. Returns 1 and fills b/d/f on the first
 * match, else 0. */
static int i915_scan_present(const struct pci_driver *drv, unsigned char *b, unsigned char *d, unsigned char *f)
{
	const struct pci_device_id *id;

	if (!drv || !drv->id_table)
		return 0;
	for (id = drv->id_table; id->vendor || id->device; id++) {
		unsigned short vend = (id->vendor == (unsigned)PCI_ANY_ID) ? PCI_VENDOR_INTEL : (unsigned short)id->vendor;
		if (id->device == (unsigned)PCI_ANY_ID)
			continue;   /* class-only wildcards: knx_pci_find needs a concrete device id */
		if (knx_pci_find(vend, (unsigned short)id->device, b, d, f))
			return 1;
	}
	return 0;
}

int nkext_init(void)
{
	const struct pci_driver *drv;
	unsigned char bus, dev, func;
	int ret;

	/* 1) LinuxKPI mem_map first — indexed by every alloc_pages/virt_to_page below. */
	{ extern void lkpi_mem_map_init(void); lkpi_mem_map_init(); }

	/* Keep DRM debug quiet (each drm_dbg -> printk -> fbcon would repaint the console). */
	{ extern unsigned long __drm_debug; __drm_debug = 0x0; }

	/* 2) DRM core (chrdev/class + drm_core_init_complete) before any probe. */
	__lkpi_modinit_drm_core_init();

	/* 3) run the UNMODIFIED i915_init(): registers the pci_driver + module subfuncs. */
	ret = __lkpi_modinit_i915_init();
	if (ret) {
		knx_log("i915: i915_init() failed\n");
		return -1;
	}
	drv = lkpi_pci_get_driver();
	if (!drv || !drv->probe) {
		knx_log("i915: driver did not register a probe\n");
		return -1;
	}
	knx_log("i915: unmodified Linux 6.12 i915 driver registered\n");

	/* 4) match the id_table against present devices. */
	if (!i915_scan_present(drv, &bus, &dev, &func)) {
		knx_log("i915: no Intel GPU present — idle (expected on QEMU; Dell bring-up is Phase B)\n");
		return 0;
	}

	/* Device present (real hardware): bring up async workqueues, then hand off to Phase B.
	 * The full pci_dev construction (BAR64 mapping, MSI, execlists) + i915 probe() lands in the
	 * Dell bring-up increment — reaching this line on the Latitude is the goal that unblocks it. */
	lkpi_wq_init();
	knx_log("i915: Intel GPU found — pci_dev construction + probe() is Phase B (Dell)\n");
	return 0;
}
