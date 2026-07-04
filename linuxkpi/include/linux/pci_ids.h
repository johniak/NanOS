/*
 * linuxkpi/include/linux/pci_ids.h — the handful of PCI vendor/subsystem IDs i915 references (the
 * full upstream list isn't vendored). Values match Linux's <linux/pci_ids.h>. PCI_VENDOR_ID /
 * PCI_DEVICE_ID config-space *offsets* live in <linux/pci.h> and are unrelated to these ID values.
 */
#ifndef _LKPI_LINUX_PCI_IDS_H
#define _LKPI_LINUX_PCI_IDS_H

#define PCI_VENDOR_ID_INTEL              0x8086
#define PCI_SUBVENDOR_ID_REDHAT_QUMRANET 0x1af4
#define PCI_SUBDEVICE_ID_QEMU            0x1100

#endif /* _LKPI_LINUX_PCI_IDS_H */
