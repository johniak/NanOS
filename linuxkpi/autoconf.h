/*
 * linuxkpi/autoconf.h — minimal Kconfig surface for the LinuxKPI build (force-included).
 * Stands in for the kernel's generated include/generated/autoconf.h. Only the options the
 * vendored virtio_gpu + DRM/virtio core actually test are enabled; everything else stays
 * undefined so the unused Linux code paths compile out.
 */
#ifndef _LINUXKPI_AUTOCONF_H
#define _LINUXKPI_AUTOCONF_H

#define CONFIG_64BIT 1
#define CONFIG_X86 1
#define CONFIG_X86_64 1
#define CONFIG_PCI 1
#define CONFIG_VIRTIO 1
#define CONFIG_VIRTIO_PCI 1
#define CONFIG_DRM 1
#define CONFIG_DRM_VIRTIO_GPU 1
#define CONFIG_DRM_VIRTIO_GPU_KMS 1

#endif /* _LINUXKPI_AUTOCONF_H */
