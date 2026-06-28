/*
 * linuxkpi/include/linux/mod_devicetable.h — device-id tables the shim needs.
 */
#ifndef _LINUXKPI_LINUX_MOD_DEVICETABLE_H
#define _LINUXKPI_LINUX_MOD_DEVICETABLE_H

#include <linux/types.h>

struct virtio_device_id {
	__u32 device;
	__u32 vendor;
};
#define VIRTIO_DEV_ANY_ID 0xffffffff

struct pci_device_id {
	__u32 vendor, device;
	__u32 subvendor, subdevice;
	__u32 class, class_mask;
	unsigned long driver_data;
};

struct of_device_id {
	char compatible[128];
	const void *data;
};

#endif /* _LINUXKPI_LINUX_MOD_DEVICETABLE_H */
