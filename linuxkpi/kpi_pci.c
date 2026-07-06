/*
 * linuxkpi/kpi_pci.c — PCI capability walking + BAR mapping + id fill for the shim,
 * backed by the knx_pci_* config accessors and knx_map_mmio.
 */
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/interrupt.h>   /* lkpi_irq_bind_msi */
#include "lkpi_knx.h"

/* Bind a single-vector MSI for this function and record the shim irq in dev->irq (see the header).
 * i915 calls this in i915_driver_hw_probe; intel_irq_install then request_irq()s dev->irq. */
int pci_enable_msi(struct pci_dev *dev) {
	int irq;
	if (!dev)
		return -1;
	irq = lkpi_irq_bind_msi(dev->nbus, dev->ndev, dev->nfunc);
	if (irq < 0)
		return -1;   /* no MSI cap / no free slot — caller keeps the legacy line */
	dev->irq = (unsigned int)irq;
	dev->msi_enabled = 1;
	return 0;
}

/* Build a pci_dev on demand for the device at (bus, devfn), or NULL if no device is present there.
 * The shim has no PCI device registry; i915 calls this to grab the host bridge (00:00.0) for GMCH /
 * MCHBAR config access (intel_gmch_bridge_setup). pci_dev_put is a no-op, so the small allocation
 * leaks — acceptable: probe calls this a handful of times, once per bring-up. */
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn) {
	unsigned char b = (unsigned char)bus;
	unsigned char d = (unsigned char)PCI_SLOT(devfn);
	unsigned char f = (unsigned char)PCI_FUNC(devfn);
	u32 v0 = knx_pci_cfg_read32(b, d, f, 0x00);
	if (v0 == 0xffffffffu || (v0 & 0xffff) == 0xffff)
		return 0;   /* no device at that slot */

	struct pci_dev *p = (struct pci_dev *)knx_malloc(sizeof *p);
	if (!p)
		return 0;
	memset(p, 0, sizeof *p);
	p->nbus = b; p->ndev = d; p->nfunc = f;
	p->devfn = (unsigned int)devfn;

	/* A minimal owning bus (pci_domain_nr / pci_bus_alloc_resource read it). */
	struct pci_bus *pb = (struct pci_bus *)knx_malloc(sizeof *pb);
	if (pb) { memset(pb, 0, sizeof *pb); pb->number = b; pb->domain_nr = domain; }
	p->bus = pb;

	lkpi_pci_fill_ids(p);
	for (int i = 0; i < 6; i++) {
		unsigned long start = pci_resource_start(p, i);
		unsigned long len   = pci_resource_len(p, i);
		p->resource[i].start = start;
		p->resource[i].end   = len ? start + len - 1 : 0;
		p->resource[i].flags = len ? pci_resource_flags(p, i) : 0;
	}
	return p;
}

/* Scan config space for the next device whose 24-bit class code matches `class`, after `from`
 * (or from the start if `from` is NULL); return a hand-built pci_dev or NULL when none remain.
 * i915's intel_detect_pch() calls pci_get_class(PCI_CLASS_BRIDGE_ISA << 8, ...) to find the PCH
 * LPC bridge (00:1f.0 on the Dell) — without this the PCH goes undetected, pch_type stays
 * PCH_NONE, HAS_PCH_SPLIT is false, and intel_pps picks the wrong PP register base (0x61200
 * instead of the PCH's 0xC7200), so panel-power writes land in dead MMIO and eDP never comes up.
 * Bounded full 0000:00..ff bus scan (probe-time, once); pci_dev_put is a no-op so the build leaks. */
struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from) {
	/* Linear bus/dev/func key so `from` can resume the scan just past the previous match. */
	unsigned int start = 0;
	if (from)
		start = ((unsigned)from->nbus << 8 | (unsigned)from->ndev << 3 | (unsigned)from->nfunc) + 1;

	for (unsigned int key = start; key < (256u << 8); key++) {
		unsigned char b = (unsigned char)(key >> 8);
		unsigned char d = (unsigned char)((key >> 3) & 0x1f);
		unsigned char f = (unsigned char)(key & 0x07);
		u32 v0 = knx_pci_cfg_read32(b, d, f, 0x00);
		if (v0 == 0xffffffffu || (v0 & 0xffff) == 0xffff)
			continue;   /* no device at that slot/func */
		/* Config 0x08: [baseclass:24][subclass:16][progif:8][revision:0]; class code = v >> 8. */
		u32 cc = knx_pci_cfg_read32(b, d, f, 0x08) >> 8;
		if (cc != class)
			continue;
		return pci_get_domain_bus_and_slot(0, b, PCI_DEVFN(d, f));
	}
	return 0;
}

/* Fill vendor/device/subsystem/revision from config space for a hand-built pci_dev whose
 * nbus/ndev/nfunc are already set. */
void lkpi_pci_fill_ids(struct pci_dev *d) {
	u32 v0 = knx_pci_cfg_read32(d->nbus, d->ndev, d->nfunc, 0x00);
	d->vendor = (u16)(v0 & 0xffff);
	d->device = (u16)(v0 >> 16);
	u32 rev = knx_pci_cfg_read32(d->nbus, d->ndev, d->nfunc, 0x08);
	d->revision = (u8)(rev & 0xff);
	u32 sub = knx_pci_cfg_read32(d->nbus, d->ndev, d->nfunc, PCI_SUBSYSTEM_VENDOR_ID);
	d->subsystem_vendor = (u16)(sub & 0xffff);
	d->subsystem_device = (u16)(sub >> 16);
	d->irq = knx_pci_irq(d->nbus, d->ndev, d->nfunc);
	d->dev.init_name = 0;
}

int pci_find_next_capability(struct pci_dev *d, u8 pos, int cap) {
	int ttl = 48;  /* loop guard */
	u8 id, next;
	u8 p;
	/* advance one entry past `pos` */
	pci_read_config_byte(d, pos + PCI_CAP_LIST_NEXT, &next);
	p = next;
	while (p && ttl--) {
		pci_read_config_byte(d, p + PCI_CAP_LIST_ID, &id);
		if (id == 0xff)
			break;
		if (id == cap)
			return p;
		pci_read_config_byte(d, p + PCI_CAP_LIST_NEXT, &next);
		p = next;
	}
	return 0;
}

int pci_find_capability(struct pci_dev *d, int cap) {
	u16 status;
	u8 pos;
	pci_read_config_word(d, PCI_STATUS, &status);
	if (!(status & PCI_STATUS_CAP_LIST))
		return 0;
	pci_read_config_byte(d, PCI_CAPABILITY_LIST, &pos);
	int ttl = 48;
	u8 id, next;
	while (pos && ttl--) {
		pci_read_config_byte(d, pos + PCI_CAP_LIST_ID, &id);
		if (id == 0xff)
			break;
		if (id == cap)
			return pos;
		pci_read_config_byte(d, pos + PCI_CAP_LIST_NEXT, &next);
		pos = next;
	}
	return 0;
}

void *pci_iomap_range(struct pci_dev *d, int bar, unsigned long offset, unsigned long maxlen) {
	unsigned long start = pci_resource_start(d, bar);
	unsigned long rlen = pci_resource_len(d, bar);
	if (!start || !rlen)
		return 0;
	if (offset > rlen)
		return 0;
	/* maxlen is the maximum number of bytes to map STARTING AT offset (0 == to end). */
	unsigned long maplen = rlen - offset;
	if (maxlen && maxlen < maplen)
		maplen = maxlen;
	return knx_map_mmio((unsigned long long)(start + offset), (unsigned long long)maplen);
}

void pci_iounmap(struct pci_dev *d, void *addr) {
	(void)d; (void)addr;   /* knx_map_mmio regions are not unmapped */
}

/* pci_register_driver: the shim keeps a SINGLE registered driver (the kext runs exactly one
 * DRM driver). i915's i915_init() (module_init) calls this via i915_pci_register_driver; the
 * kext entry then retrieves it with lkpi_pci_get_driver() and drives probe() itself against a
 * hand-built pci_dev — the port equivalent of the PCI bus match/probe in drivers/pci/pci-driver.c. */
static struct pci_driver *g_pci_drv;

int pci_register_driver(struct pci_driver *drv) {
	g_pci_drv = drv;
	return 0;
}

void pci_unregister_driver(struct pci_driver *drv) {
	if (g_pci_drv == drv)
		g_pci_drv = 0;
}

struct pci_driver *lkpi_pci_get_driver(void) {
	return g_pci_drv;
}
