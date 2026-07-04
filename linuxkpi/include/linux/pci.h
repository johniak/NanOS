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
#include <linux/pci_ids.h>   /* PCI_VENDOR_ID_INTEL etc. (Linux's pci.h pulls the ID list too) */
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/pm.h>   /* pm_message_t for pci_choose_state */
#include <linux/ioport.h>  /* struct resource — pci_dev embeds a BAR resource array by value */
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
	struct pci_bus *bus;
	unsigned int devfn;
	void *priv;
	unsigned int msi_enabled:1;
	unsigned int msix_enabled:1;
	unsigned int no_64bit_msi:1;
	unsigned int current_state;
	struct resource resource[7];   /* BAR windows (6 BARs + ROM); i915 GSC reads resource[0] */
};
struct pci_bus { unsigned char number; int domain_nr; struct pci_bus *parent; struct pci_dev *self; struct resource *resource[4]; };
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_DEVID(bus, devfn) ((((unsigned)(bus)) << 8) | (devfn))
#define PCI_BUS_NUM(devid)    (((devid) >> 8) & 0xff)
static inline int pci_domain_nr(struct pci_bus *b){ return b ? b->domain_nr : 0; }

struct pci_device_id;
struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	void (*shutdown)(struct pci_dev *dev);
	struct device_driver driver;   /* i915_pci sets .driver.pm = &i915_pm_ops */
	void *driver_management;
};

#ifdef __cplusplus
extern "C" {
#endif
/* driver registration: the kext bootstrap calls lkpi_module_init (module_pci_driver) which calls
 * pci_register_driver; the shim keeps the single driver and probes our GPU directly (kpi_pci.c). */
int  pci_register_driver(struct pci_driver *drv);
void pci_unregister_driver(struct pci_driver *drv);
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
/* word/byte config writes: read-modify-write the containing dword (config space is dword-addressed). */
static inline int pci_write_config_word(struct pci_dev *d, int where, u16 val) {
	u32 v = knx_pci_cfg_read32(d->nbus, d->ndev, d->nfunc, (unsigned char)(where & ~3));
	int sh = (where & 2) * 8; v = (v & ~(0xffffu << sh)) | ((u32)val << sh);
	knx_pci_cfg_write32(d->nbus, d->ndev, d->nfunc, (unsigned char)(where & ~3), v); return 0;
}
static inline int pci_write_config_byte(struct pci_dev *d, int where, u8 val) {
	u32 v = knx_pci_cfg_read32(d->nbus, d->ndev, d->nfunc, (unsigned char)(where & ~3));
	int sh = (where & 3) * 8; v = (v & ~(0xffu << sh)) | ((u32)val << sh);
	knx_pci_cfg_write32(d->nbus, d->ndev, d->nfunc, (unsigned char)(where & ~3), v); return 0;
}
/* refcount put on a pci_dev: the shim doesn't refcount pci_dev handles, so this is a no-op. */
static inline void pci_dev_put(struct pci_dev *d) { (void)d; }
/* Option-ROM mapping: unused on the KMS path (VBT comes from the OpRegion/ACPI, not the PCI ROM BAR). */
static inline void __iomem *pci_map_rom(struct pci_dev *d, size_t *size) { (void)d; if (size) *size = 0; return 0; }
static inline void pci_unmap_rom(struct pci_dev *d, void __iomem *rom) { (void)d; (void)rom; }
/* Standard BAR resource index range (Linux keeps these in an enum in <linux/pci.h>). */
#ifndef PCI_STD_RESOURCES
#define PCI_STD_RESOURCES    0
#define PCI_STD_RESOURCE_END 5
#define PCI_STD_NUM_BARS     6
#define PCI_ROM_RESOURCE     6
#endif

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


/* PCI power states (intel_opregion references pci_power_t). */
typedef int pci_power_t;
#define PCI_D0     0
#define PCI_D1     1
#define PCI_D2     2
#define PCI_D3hot  3
#define PCI_D3cold 4
#define PCI_POWER_ERROR (-1)

/* bus-level config accessors: address by (bus, devfn) instead of a pci_dev. Same knx_pci path. */
static inline int pci_bus_read_config_dword(struct pci_bus *b, unsigned int devfn, int where, u32 *val) {
	*val = knx_pci_cfg_read32(b?b->number:0, PCI_SLOT(devfn), PCI_FUNC(devfn), (unsigned char)where); return 0;
}
static inline int pci_bus_read_config_word(struct pci_bus *b, unsigned int devfn, int where, u16 *val) {
	u32 v = knx_pci_cfg_read32(b?b->number:0, PCI_SLOT(devfn), PCI_FUNC(devfn), (unsigned char)(where & ~3));
	*val = (u16)(v >> ((where & 2) * 8)); return 0;
}
static inline int pci_bus_read_config_byte(struct pci_bus *b, unsigned int devfn, int where, u8 *val) {
	u32 v = knx_pci_cfg_read32(b?b->number:0, PCI_SLOT(devfn), PCI_FUNC(devfn), (unsigned char)(where & ~3));
	*val = (u8)(v >> ((where & 3) * 8)); return 0;
}
static inline int pci_bus_write_config_word(struct pci_bus *b, unsigned int devfn, int where, u16 val) {
	u32 v = knx_pci_cfg_read32(b?b->number:0, PCI_SLOT(devfn), PCI_FUNC(devfn), (unsigned char)(where & ~3));
	int sh = (where & 2) * 8; v = (v & ~(0xffffu << sh)) | ((u32)val << sh);
	knx_pci_cfg_write32(b?b->number:0, PCI_SLOT(devfn), PCI_FUNC(devfn), (unsigned char)(where & ~3), v); return 0;
}
static inline int pci_bus_write_config_byte(struct pci_bus *b, unsigned int devfn, int where, u8 val) {
	u32 v = knx_pci_cfg_read32(b?b->number:0, PCI_SLOT(devfn), PCI_FUNC(devfn), (unsigned char)(where & ~3));
	int sh = (where & 3) * 8; v = (v & ~(0xffu << sh)) | ((u32)val << sh);
	knx_pci_cfg_write32(b?b->number:0, PCI_SLOT(devfn), PCI_FUNC(devfn), (unsigned char)(where & ~3), v); return 0;
}
/* PCI resizable-BAR: report the size encoded by a rebar ctrl value as bytes (1MiB << index). */
static inline unsigned long pci_rebar_bytes_to_size(unsigned long bytes){ unsigned long o = 20 /*1MiB*/; while ((1UL<<o) < bytes) o++; return o - 20; }
static inline int pci_bus_alloc_resource(struct pci_bus *b, void *res, unsigned long size, unsigned long align, unsigned long min, unsigned long type, void *alignf, void *alignf_data){ (void)b;(void)res;(void)size;(void)align;(void)min;(void)type;(void)alignf;(void)alignf_data; return -6; }
/* PCI_DEVICE(vend, dev): initialize a pci_device_id matching any subsystem/class. */
#define PCI_DEVICE(vend, dev) .vendor = (vend), .device = (dev), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_CLASS(dev_class, dev_class_mask) .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, .class = (dev_class), .class_mask = (dev_class_mask)

/* Device lookup helpers. The shim probes only our single GPU (handed to it directly), so scans for
 * OTHER devices (bridges, ISA, another GPU) find nothing: return NULL. Callers treat NULL as absent. */
static inline struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from) { (void)class;(void)from; return 0; }
static inline struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn) { (void)domain;(void)bus;(void)devfn; return 0; }
/* pci_match_id: linear scan of a null-terminated id table for a vendor/device match (real logic). */
static inline const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev) {
	if (!ids || !dev) return 0;
	for (; ids->vendor || ids->device || ids->subvendor || ids->class_mask; ids++) {
		if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor) &&
		    (ids->device == PCI_ANY_ID || ids->device == dev->device) &&
		    (ids->subvendor == PCI_ANY_ID || ids->subvendor == dev->subsystem_vendor) &&
		    (ids->subdevice == PCI_ANY_ID || ids->subdevice == dev->subsystem_device))
			return ids;
	}
	return 0;
}
/* Option-ROM / BAR resource release: the shim doesn't reserve them, so releasing is a no-op. */
static inline void pci_release_resource(struct pci_dev *dev, int bar) { (void)dev; (void)bar; }
static inline int pci_resource_n(struct pci_dev *dev) { (void)dev; return PCI_STD_NUM_BARS; }
/* resizable-BAR resize: the shim can't re-negotiate BAR size, so report unsupported. */
static inline int pci_resize_resource(struct pci_dev *dev, int bar, int size){ (void)dev;(void)bar;(void)size; return -95; }
/* is a device matching this id table currently present? Only our GPU is; others are absent. */
static inline int pci_dev_present(const struct pci_device_id *ids){ (void)ids; return 0; }
static inline void pci_assign_unassigned_bus_resources(struct pci_bus *bus){ (void)bus; }
static inline int pci_set_power_state(struct pci_dev *dev, pci_power_t state){ (void)dev;(void)state; return 0; }
static inline pci_power_t pci_choose_state(struct pci_dev *dev, pm_message_t state){ (void)dev;(void)state; return PCI_D0; }
static inline int pci_save_state(struct pci_dev *dev){ (void)dev; return 0; }
static inline void pci_restore_state(struct pci_dev *dev){ (void)dev; }
static inline int pci_enable_device_mem(struct pci_dev *dev){ (void)dev; return 0; }
/* PCIe topology walk: the shim exposes a flat single-device view, so the "root port" is the device
 * itself (i915 only reads capabilities off it). */
static inline struct pci_dev *pcie_find_root_port(struct pci_dev *dev){ return dev; }
static inline struct pci_dev *pci_upstream_bridge(struct pci_dev *dev){ (void)dev; return 0; }
static inline int pcie_get_readrq(struct pci_dev *dev){ (void)dev; return 512; }
static inline int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val){ (void)dev;(void)pos; if(val)*val=0; return 0; }
static inline void pci_d3cold_disable(struct pci_dev *dev){ (void)dev; }
static inline void pci_d3cold_enable(struct pci_dev *dev){ (void)dev; }
static inline void pci_ignore_hotplug(struct pci_dev *dev){ (void)dev; }
static inline int pci_pcie_type(const struct pci_dev *dev){ (void)dev; return 0; }
/* resizable-BAR possible-size bitmask: BAR is fixed in the shim, so only the current size is offered. */
static inline u32 pci_rebar_get_possible_sizes(struct pci_dev *dev, int bar){ (void)dev;(void)bar; return 0; }
/* MSI enable: interrupts are wired by the kext's minimal LAPIC path (single vector), so report OK. */
static inline int pci_enable_msi(struct pci_dev *dev){ (void)dev; return 0; }
static inline void pci_disable_msi(struct pci_dev *dev){ (void)dev; }
/* pcibios_align_resource: identity alignment (return the requested start unchanged). */
static inline unsigned long pcibios_align_resource(void *data, const struct resource *res, unsigned long size, unsigned long align){ (void)data;(void)res;(void)size;(void)align; return 0; }
/* iterate a bus's window resources (shim bus has 4 slots, mostly NULL). i915 6.12 passes an index var. */
#define pci_bus_for_each_resource(bus, res, i) \
	for ((i) = 0; (i) < 4 && ((res) = (bus)->resource[(i)], 1); (i)++)
#ifndef PCIBIOS_MIN_MEM
#define PCIBIOS_MIN_MEM 0x100000
#define PCIBIOS_MIN_IO  0x1000
#endif
#ifndef PCI_CLASS_BRIDGE_ISA
#define PCI_CLASS_BRIDGE_ISA   0x0601
#define PCI_CLASS_DISPLAY_VGA  0x0300
#define PCI_CLASS_BRIDGE_HOST  0x0600
#endif

#endif /* _LINUXKPI_LINUX_PCI_H */
