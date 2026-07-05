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
#include <linux/string.h>
#include <drm/intel/intel-gtt.h>
#include "i915_params.h"    /* i915_modparams — set enable_guc / inject_probe_failure before probe */
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
 * so the report reflects exactly what i915 would bind. Returns the matched id_table entry (whose
 * ->driver_data is the intel_device_info probe() needs) and fills b/d/f, else 0. */
static const struct pci_device_id *i915_scan_present(const struct pci_driver *drv,
		unsigned char *b, unsigned char *d, unsigned char *f)
{
	const struct pci_device_id *id;

	if (!drv || !drv->id_table)
		return 0;
	for (id = drv->id_table; id->vendor || id->device; id++) {
		unsigned short vend = (id->vendor == (unsigned)PCI_ANY_ID) ? PCI_VENDOR_INTEL : (unsigned short)id->vendor;
		if (id->device == (unsigned)PCI_ANY_ID)
			continue;   /* class-only wildcards: knx_pci_find needs a concrete device id */
		if (knx_pci_find(vend, (unsigned short)id->device, b, d, f))
			return id;
	}
	return 0;
}

/* ---- hand-built pci_dev (Phase B) --------------------------------------------------------------
 * On a real x86 kernel the PCI bus code builds the pci_dev and calls the driver's probe on a match.
 * Our port has no PCI-bus/driver-model, so we construct the pci_dev the driver needs by hand from
 * the enumerated address, then call probe() ourselves. The shim's config-space/BAR accessors all
 * key off nbus/ndev/nfunc, so filling those + the id fields + the resource[] windows is enough for
 * i915's uncore/GGTT setup to read the real hardware. */
static struct pci_dev g_i915_pdev;
static struct pci_bus g_i915_bus;
static u64 g_i915_dma_mask = ~0ULL;   /* 64-bit; the shim's dma_set_* are no-ops but paths may read it */

static struct pci_dev *i915_build_pci_dev(unsigned char b, unsigned char d, unsigned char f)
{
	struct pci_dev *p = &g_i915_pdev;
	int i;

	memset(p, 0, sizeof *p);
	memset(&g_i915_bus, 0, sizeof g_i915_bus);
	p->nbus = b; p->ndev = d; p->nfunc = f;
	p->devfn = PCI_DEVFN(d, f);
	g_i915_bus.number = b;
	g_i915_bus.domain_nr = 0;
	p->bus = &g_i915_bus;

	/* vendor/device/subsystem/revision + legacy IRQ line, straight from config space. */
	lkpi_pci_fill_ids(p);

	/* BAR windows: some i915 paths read pdev->resource[bar] directly rather than via
	 * pci_resource_start(). Decode all six from live config space (a 64-bit BAR's high slot reads
	 * back 0 — Pci::readBars folds it into the low slot — so its resource entry is correctly empty). */
	for (i = 0; i < 6; i++) {
		unsigned long start = pci_resource_start(p, i);
		unsigned long len   = pci_resource_len(p, i);
		p->resource[i].start = start;
		p->resource[i].end   = len ? start + len - 1 : 0;
		p->resource[i].flags = len ? pci_resource_flags(p, i) : 0;
	}

	/* A 64-bit DMA mask (IGP shares system RAM, DMA is identity-mapped). */
	p->dev.dma_mask = &g_i915_dma_mask;
	p->dev.coherent_dma_mask = ~0ULL;
	p->dev.init_name = "i915";
	return p;
}

/* Optional dial-a-stop: /nanos/config/i915_inject holds an integer N. N>0 -> i915_modparams.
 * inject_probe_failure = N, making probe abort via the driver's OWN clean -ENODEV unwind at its Nth
 * internal injection point. After a full-probe crash you dial N down to the last clean stage to
 * confirm the unwind path, with NO rebuild/reflash. Absent/0 = full probe. */
#define I915_INJECT_KNOB "/disks/main/nanos/config/i915_inject"
static unsigned i915_inject_stop(void)
{
	char buf[8];
	unsigned long n = 0, v = 0;
	const char *p;
	if (knx_file_read(I915_INJECT_KNOB, buf, sizeof(buf) - 1, &n) < 0)
		return 0;
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = 0;
	for (p = buf; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (unsigned)(*p - '0');
	return (unsigned)v;
}

/* ---- bring-up debug harness ---------------------------------------------------------------- *
 * The Dell Latitude 5310 has NO serial port and every iteration is a USB re-flash, so the design
 * goal is "one boot tells us as much as possible". Two log channels, always in lockstep:
 *   - knx_log -> the firmware fbcon (guaranteed visible even on a hard hang — photograph it),
 *   - knx_file_append -> /nanos/logs/i915-boot.txt on the writable root (survives the reboot).
 * i915 is ARMED only when /nanos/config/i915 begins with '1'. Unarmed, nkext_init returns BEFORE
 * touching mem_map / DRM core / i915_init, so the kext is a safe no-op in the default image (and
 * cannot double-init DRM core against virtio_gpu). Armed, it turns the vendored DRM debug all the
 * way up and narrates every stage. */
/* The physical (USB/ATA) root is mounted at /disks/main — "/" itself is an in-RAM SynthFs — so the
 * PERSISTENT paths kexts see are under /disks/main/nanos/... (this is exactly where loadAllKexts
 * reads the .nkext from). /disks/main is mounted read-write before the kexts load. */
#define I915_ARM_KNOB "/disks/main/nanos/config/i915"
#define I915_LOG_PATH "/disks/main/nanos/logs/i915-boot.txt"

static unsigned long i915_strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

/* Tee one marker to both channels. Keep messages short + prefixed "i915:" so a photo or a grep of
 * the log reads as a timeline. */
static void i915_log(const char *msg)
{
	knx_log(msg);
	knx_file_append(I915_LOG_PATH, msg, i915_strlen(msg));
}

/* Armed iff the first non-space byte of /nanos/config/i915 is '1'. Absent/unreadable/0 -> disarmed. */
static int i915_armed(void)
{
	char buf[8];
	unsigned long n = 0;
	const char *p;
	if (knx_file_read(I915_ARM_KNOB, buf, sizeof(buf) - 1, &n) < 0)
		return 0;
	buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = 0;
	for (p = buf; *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'; p++)
		;
	return *p == '1';
}

/* Tee a marker plus a small integer (probe return code / inject stage) to both channels — knx_log /
 * knx_file_append take strings, so format the number here. */
static void i915_log_val(const char *msg, long v)
{
	char num[24], rev[24];
	int i = 0, neg = 0, j = 0;
	unsigned long u;
	if (v < 0) { neg = 1; u = (unsigned long)(-v); } else u = (unsigned long)v;
	if (u == 0) rev[i++] = '0';
	while (u) { rev[i++] = (char)('0' + u % 10); u /= 10; }
	if (neg) num[j++] = '-';
	while (i) num[j++] = rev[--i];
	num[j++] = '\n'; num[j] = 0;
	knx_log(msg); knx_log(num);
	knx_file_append(I915_LOG_PATH, msg, i915_strlen(msg));
	knx_file_append(I915_LOG_PATH, num, i915_strlen(num));
}

int nkext_init(void)
{
	const struct pci_driver *drv;
	const struct pci_device_id *id;
	unsigned char bus, dev, func;
	int ret;

	/* 0) arm gate. Disarmed = a safe no-op (default image); nothing below runs. */
	if (!i915_armed()) {
		knx_log("i915: not armed (write '1' to " I915_ARM_KNOB " to bring up) — skipping\n");
		return 0;
	}

	i915_log("i915: ===== bring-up session armed =====\n");

	/* Route EVERY subsequent printk (incl. the full drm_dbg trail once __drm_debug is up) into the
	 * persistent log too, so a probe that scrolls the fbcon or hard-hangs still leaves the complete
	 * narration on the stick (recover with `make i915-log`). */
	lkpi_set_log_tee(I915_LOG_PATH);

	/* 1) LinuxKPI mem_map first — indexed by every alloc_pages/virt_to_page below. */
	{ extern void lkpi_mem_map_init(void); lkpi_mem_map_init(); }
	i915_log("i915: mem_map init OK\n");

	/* Turn the vendored DRM debug all the way up (CORE|DRIVER|KMS|PRIME|ATOMIC|VBL|STATE|LEASE|DP):
	 * every drm_dbg/atomic-state dump now flows through printk -> knx_log -> both channels. This is
	 * exactly the drm.debug=0xff a normal kernel would take for a bring-up, and it is why one armed
	 * boot is worth many blind ones. */
	{ extern unsigned long __drm_debug; __drm_debug = 0x1ff; }

	/* 2) DRM core (chrdev/class + drm_core_init_complete) before any probe. */
	__lkpi_modinit_drm_core_init();
	i915_log("i915: DRM core init OK\n");

	/* 3) run the UNMODIFIED i915_init(): registers the pci_driver + module subfuncs. */
	ret = __lkpi_modinit_i915_init();
	if (ret) {
		i915_log("i915: FAIL i915_init()\n");
		return 0;   /* never fault the box — firmware fb stays up */
	}
	drv = lkpi_pci_get_driver();
	if (!drv || !drv->probe) {
		i915_log("i915: FAIL driver registered no probe\n");
		return 0;
	}
	i915_log("i915: unmodified Linux 6.12 i915 driver registered\n");

	/* 4) match the id_table against present devices. */
	id = i915_scan_present(drv, &bus, &dev, &func);
	if (!id) {
		i915_log("i915: no Intel GPU present — idle (expected on QEMU; Dell bring-up is Phase B)\n");
		return 0;
	}

	/* Device present (real hardware). Bring up async workqueues (breadcrumb/hangcheck/retire
	 * workers), then hand-build the pci_dev and drive i915's own probe() — Phase B (Dell). */
	lkpi_wq_init();

	/* Gen9.5 (Comet Lake-U) runs GuC-less on execlists; force it explicit (the default -1 auto-selects
	 * the same on Gen9, but a firmware-less box then never blocks waiting on a GuC load). */
	i915_modparams.enable_guc = 0;

	/* Optional dial-a-stop: abort probe via i915's own clean unwind at injection point N (see the
	 * knob comment). 0 = full probe. Lets a crash be walked back to the last clean stage w/o rebuild. */
	i915_modparams.inject_probe_failure = i915_inject_stop();
	if (i915_modparams.inject_probe_failure)
		i915_log_val("i915: inject_probe_failure armed at stage ", (long)i915_modparams.inject_probe_failure);

	i915_log_val("i915: Intel GPU FOUND at bus ", (long)bus);
	{
		struct pci_dev *pdev = i915_build_pci_dev(bus, dev, func);
		int pret;

		/* Pre-probe hardware inventory — config-space only (safe, no forcewake), so this is captured
		 * no matter how far probe gets and completes the Task-1 table the Dell can't print (no serial):
		 * the BAR windows we handed the driver + the graphics config regs (GGC stolen size, BDSM stolen
		 * base, ASLS OpRegion pointer) + whether an MSI capability is present. */
		i915_log_val("i915:   BAR0 (GTTMMADR) start ", (long)pdev->resource[0].start);
		i915_log_val("i915:   BAR0 size            ", (long)pci_resource_len(pdev, 0));
		i915_log_val("i915:   BAR2 (GMADR)    start ", (long)pdev->resource[2].start);
		i915_log_val("i915:   BAR2 size            ", (long)pci_resource_len(pdev, 2));
		i915_log_val("i915:   legacy IRQ line       ", (long)pdev->irq);
		i915_log_val("i915:   GGC  (0x50, stolen)   ", (long)(knx_pci_cfg_read32(bus, dev, func, 0x50) & 0xffff));
		i915_log_val("i915:   BDSM (0x5C, stolen@)  ", (long)knx_pci_cfg_read32(bus, dev, func, 0x5C));
		i915_log_val("i915:   ASLS (0xFC, OpRegion) ", (long)knx_pci_cfg_read32(bus, dev, func, 0xFC));
		i915_log_val("i915:   MSI cap offset        ", (long)pci_find_capability(pdev, 0x05 /*PCI_CAP_ID_MSI*/));

		i915_log("i915: probe start — narrating via drm_dbg (see log tail)\n");
		pret = drv->probe(pdev, id);
		i915_log_val("i915: probe RETURNED ", (long)pret);

		/* Post-probe hardware-state snapshot (captured regardless of how far probe got, so one boot
		 * yields the interrupt/bus-master truth even on a failure): the bound MSI irq (>=32 means
		 * pci_enable_msi programmed a vector; 255 = still legacy), and the PCI command register
		 * (bit1=mem-space, bit2=bus-master, bit10=INTx-disable). */
		i915_log_val("i915:   post: pdev->irq   ", (long)pdev->irq);
		i915_log_val("i915:   post: msi_enabled ", (long)pdev->msi_enabled);
		i915_log_val("i915:   post: PCI cmd reg ", (long)(knx_pci_cfg_read32(bus, dev, func, 0x04) & 0xffff));
		if (pret == 0)
			i915_log("i915: DRIVER BOUND — GPU is up (full probe succeeded)\n");
		else if (pret == -ENODEV && i915_modparams.inject_probe_failure)
			i915_log("i915: stopped at the armed inject point (clean -ENODEV unwind)\n");
		else
			i915_log("i915: probe FAILED — see the drm_dbg trail above for the failing stage\n");
	}
	return 0;
}
