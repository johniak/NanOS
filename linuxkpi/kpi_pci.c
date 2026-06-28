/*
 * linuxkpi/kpi_pci.c — PCI capability walking + BAR mapping + id fill for the shim,
 * backed by the knx_pci_* config accessors and knx_map_mmio.
 */
#include <linux/pci.h>
#include "lkpi_knx.h"

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
	unsigned long len = pci_resource_len(d, bar);
	if (!start || !len)
		return 0;
	if (maxlen && maxlen < len)
		len = maxlen;
	if (offset >= len)
		return 0;
	return knx_map_mmio((unsigned)(start + offset), (unsigned)(len - offset));
}

void pci_iounmap(struct pci_dev *d, void *addr) {
	(void)d; (void)addr;   /* knx_map_mmio regions are not unmapped */
}
