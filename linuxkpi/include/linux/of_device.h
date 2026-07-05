/*
 * linuxkpi/include/linux/of_device.h — Open Firmware / device-tree device helpers.
 *
 * NanOS/x86 has no device tree, so every of_* lookup reports "no node". drm_mipi_dsi.c
 * pulls this for DT-based DSI host/device binding, which never fires on ACPI/x86 — the
 * DSI host is registered by the driver directly. Routes to the existing <linux/of.h>
 * shim and adds the handful of of_device.h-specific helpers it references.
 */
#ifndef _LKPI_OF_DEVICE_H
#define _LKPI_OF_DEVICE_H
#include <linux/of.h>
#include <linux/device.h>

static inline const void *of_device_get_match_data(const struct device *dev){ (void)dev; return 0; }
static inline int of_driver_match_device(struct device *dev, const struct device_driver *drv){ (void)dev;(void)drv; return 0; }
static inline int of_device_uevent_modalias(const struct device *dev, struct kobj_uevent_env *env){ (void)dev;(void)env; return -ENODEV; }

#endif
