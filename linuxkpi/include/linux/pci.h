/*
 * linuxkpi/include/linux/pci.h — a minimal PCI layer for the shim, backed by knx_pci_*.
 *
 * struct pci_dev carries the NanOS bus/dev/func address plus an embedded struct device.
 * Config access, BAR resources and MMIO mapping map onto the knx_pci accessors and
 * knx_map_mmio. This is the surface virtio_pci_modern_dev.c uses; bus probing, SR-IOV and
 * MSI-affinity are not lifted (the module hand-builds the device and drives the modern
 * register layer).
 */
#ifndef _LINUXKPI_LINUX_PCI_H
#define _LINUXKPI_LINUX_PCI_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <lkpi_knx.h>

#define PCI_ANY_ID (~0)
#define PCI_STD_NUM_BARS 6

/* config-space offsets used by cap walking */
#define PCI_VENDOR_ID        0x00
#define PCI_DEVICE_ID        0x02
#define PCI_COMMAND          0x04
#define PCI_STATUS           0x06
#define PCI_REVISION_ID      0x08
#define PCI_CAPABILITY_LIST  0x34
#define PCI_CAP_LIST_NEXT    1
#define PCI_CAP_LIST_ID      0
#define PCI_STATUS_CAP_LIST  0x10
#define PCI_CAP_ID_VNDR      0x09
#define PCI_CAP_ID_MSIX      0x11
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_SUBSYSTEM_ID     0x2e
#define PCI_COMMAND_MEMORY   0x2
#define PCI_COMMAND_MASTER   0x4

/* resource flags */
#define IORESOURCE_IO   0x00000100
#define IORESOURCE_MEM  0x00000200

struct pci_dev {
	struct device dev;
	unsigned char nbus, ndev, nfunc;     /* NanOS PCI address for knx_pci_* */
	unsigned short vendor;
	unsigned short device;
	unsigned short subsystem_vendor;
	unsigned short subsystem_device;
	unsigned char revision;
	unsigned int irq;
	void *priv;
};

struct pci_device_id;
struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
};

#ifdef __cplusplus
extern "C" {
#endif
/* implemented in linuxkpi/kpi_pci.c */
int   pci_find_capability(struct pci_dev *dev, int cap);
int   pci_find_next_capability(struct pci_dev *dev, u8 pos, int cap);
void *pci_iomap_range(struct pci_dev *dev, int bar, unsigned long offset, unsigned long maxlen);
void  pci_iounmap(struct pci_dev *dev, void *addr);
/* fills vendor/device/subsystem/revision from config space for a hand-built pci_dev. */
void  lkpi_pci_fill_ids(struct pci_dev *dev);
#ifdef __cplusplus
}
#endif

static inline const char *pci_name(const struct pci_dev *dev) { (void)dev; return "virtio-pci"; }
static inline void *pci_get_drvdata(struct pci_dev *dev) { return dev->priv; }
static inline void  pci_set_drvdata(struct pci_dev *dev, void *d) { dev->priv = d; }

static inline int pci_read_config_byte(struct pci_dev *d, int where, u8 *val) {
	u32 v = knx_pci_cfg_read32(d->nbus, d->ndev, d->nfunc, (unsigned char)(where & ~3));
	*val = (u8)(v >> ((where & 3) * 8)); return 0;
}
static inline int pci_read_config_word(struct pci_dev *d, int where, u16 *val) {
	u32 v = knx_pci_cfg_read32(d->nbus, d->ndev, d->nfunc, (unsigned char)(where & ~3));
	*val = (u16)(v >> ((where & 2) * 8)); return 0;
}
static inline int pci_read_config_dword(struct pci_dev *d, int where, u32 *val) {
	*val = knx_pci_cfg_read32(d->nbus, d->ndev, d->nfunc, (unsigned char)where); return 0;
}
static inline int pci_write_config_dword(struct pci_dev *d, int where, u32 val) {
	knx_pci_cfg_write32(d->nbus, d->ndev, d->nfunc, (unsigned char)where, val); return 0;
}

static inline unsigned long pci_resource_start(struct pci_dev *d, int n) { return knx_pci_bar(d->nbus, d->ndev, d->nfunc, n); }
static inline unsigned long pci_resource_len(struct pci_dev *d, int n)   { return knx_pci_bar_size(d->nbus, d->ndev, d->nfunc, n); }
static inline unsigned long pci_resource_flags(struct pci_dev *d, int n) {
	return knx_pci_bar_is_io(d->nbus, d->ndev, d->nfunc, n) ? IORESOURCE_IO : IORESOURCE_MEM;
}

static inline int  pci_enable_device(struct pci_dev *d) { knx_pci_enable_bus_master(d->nbus, d->ndev, d->nfunc); return 0; }
static inline void pci_disable_device(struct pci_dev *d) { (void)d; }
static inline void pci_set_master(struct pci_dev *d) { knx_pci_enable_bus_master(d->nbus, d->ndev, d->nfunc); }
static inline int  pci_request_selected_regions(struct pci_dev *d, int bars, const char *name) { (void)d;(void)bars;(void)name; return 0; }
static inline void pci_release_selected_regions(struct pci_dev *d, int bars) { (void)d;(void)bars; }
static inline int  pci_select_bars(struct pci_dev *d, unsigned long flags) { (void)d;(void)flags; return 0; }
static inline int  pci_device_is_present(struct pci_dev *d) { (void)d; return 1; }

static inline struct pci_dev *to_pci_dev(struct device *dev) { return container_of(dev, struct pci_dev, dev); }
static inline int dev_is_pci(struct device *dev) { (void)dev; return 1; }
static inline int pci_is_vga(struct pci_dev *d) { (void)d; return 0; }
#define dev_is_removable(dev) (false)

#endif /* _LINUXKPI_LINUX_PCI_H */
