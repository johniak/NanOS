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

/* i915 (Task 5, Dell GPU plan). The driver is built, but with a deliberately minimal feature set:
 * display + core + GEM + GT + GuC/HuC/GSC/PXP-base, and NOTHING that pulls in a subsystem NanOS
 * lacks. Each of the following is left UNDEFINED (=n) on purpose (see scripts/i915-objs.txt): GVT,
 * PXP extras, error capture, selftests, HWMON, PMU/PERF_EVENTS, DEBUG_FS, FBDEV emulation (we own
 * /dev/fb0), DP tunnelling, COMPAT (64-bit only), ACPI opregion (Phase-B follow-on). */
#define CONFIG_DRM_I915 1
#define CONFIG_DRM_I915_FENCE_TIMEOUT 10000
#define CONFIG_DRM_I915_USERFAULT_AUTOSUSPEND 250
#define CONFIG_DRM_I915_HEARTBEAT_INTERVAL 2500
#define CONFIG_DRM_I915_PREEMPT_TIMEOUT 640
#define CONFIG_DRM_I915_PREEMPT_TIMEOUT_COMPUTE 7500
#define CONFIG_DRM_I915_MAX_REQUEST_BUSYWAIT 8000
#define CONFIG_DRM_I915_STOP_TIMEOUT 100
#define CONFIG_DRM_I915_TIMESLICE_DURATION 1
/* force-probe list is empty: we probe only the real Gen9.5 IDs the driver already claims. */
#define CONFIG_DRM_I915_FORCE_PROBE ""

#endif /* _LINUXKPI_AUTOCONF_H */
