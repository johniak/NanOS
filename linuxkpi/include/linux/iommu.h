/*
 * linuxkpi/include/linux/iommu.h — IOMMU presence query (minimal).
 *
 * NanOS does not enable an IOMMU/VT-d translation layer, so DMA is identity-mapped
 * (bus address == physical address). device_iommu_mapped() reports false, which is what
 * intel-gtt.c needs (the GTT is programmed with physical page addresses directly). The
 * canonical definition lives in <linux/device.h>; route to it so we don't double-define.
 */
#ifndef _LKPI_IOMMU_H
#define _LKPI_IOMMU_H
#include <linux/device.h>
#endif
