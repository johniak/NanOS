/*
 * virtio_gpu_drv_entry.c — NanOS kext bootstrap that runs the UNMODIFIED Linux 6.12
 * virtio_gpu DRM driver.
 *
 * Unlike virtio_gpu_kext.c (the hand-written virtio-gpu protocol path), this entry does
 * NOT speak the device protocol itself. It:
 *   1. triggers the driver's own registration  (lkpi_module_init -> register_virtio_driver,
 *      emitted from the driver's unmodified module_virtio_driver() via the shim macro),
 *   2. builds a virtio_device for the PCI function (the hand-built modern transport, the
 *      port's equivalent of virtio_pci_common.c),
 *   3. calls the registered driver's .probe(vdev) — i.e. the real virtio_gpu_probe(), which
 *      brings up the full DRM/KMS device, GEM shmem objects, fences, planes and connectors,
 *   4. bridges the result to /dev/fb0.
 *
 * The virtio bus registration (register/unregister/is_virtio_device/virtio_reset_device) is
 * the LinuxKPI glue that stands in for drivers/virtio/virtio.c — the driver and DRM core
 * above it are compiled and linked verbatim.
 */
#include <linux/module.h>   /* before virtio.h: file-scope `struct module` for the proto */
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/slab.h>
#include <linux/workqueue.h>   /* lkpi_wq_init(): async workqueues + pump-drain (Task 3) */
#include "lkpi_knx.h"
#include "virtio_transport.h"

#define VIRTIO_PCI_VENDOR  0x1AF4
#define VIRTIO_GPU_PCI_DEV 0x1050

/* ---- virtio bus registration glue (stands in for drivers/virtio/virtio.c) ----------- */

static struct virtio_driver *g_virtio_drv;

int __register_virtio_driver(struct virtio_driver *drv, struct module *owner)
{
	(void)owner;
	g_virtio_drv = drv;
	return 0;
}

void unregister_virtio_driver(struct virtio_driver *drv)
{
	if (g_virtio_drv == drv)
		g_virtio_drv = 0;
}

bool is_virtio_device(struct device *dev)
{
	(void)dev;
	return true;
}

void virtio_reset_device(struct virtio_device *vdev)
{
	if (vdev && vdev->config && vdev->config->reset)
		vdev->config->reset(vdev);
}

/* The driver's module_virtio_driver() expands (via the shim macro) to this global. */
extern int lkpi_module_init(void);
/* DRM core's own module_init(drm_core_init) — emitted as this global by the shim
 * module_init macro. Must run before any drm_dev_register() (sets the chrdev/class and the
 * drm_core_init_complete flag), exactly as a postcore_initcall would in a real kernel. */
extern int __lkpi_modinit_drm_core_init(void);
/* Set by kpi_fence.c; lets the transport pump the control vq while a fence is awaited. */
extern void lkpi_set_fence_poll(void (*fn)(void));

/* Post-probe DRM modeset/scanout bring-up (drm client). Defined in virtio_gpu_present.c;
 * weak so the module links even before that stage is wired. */
__attribute__((weak)) int virtio_gpu_fbcon_bringup(struct virtio_device *vdev) { (void)vdev; return -1; }

/* Register /dev/dri/card0 + renderD128 over the DRM stack (virtio_gpu_drm_node.c). */
int virtio_gpu_drm_node_init(struct virtio_device *vdev);

static struct virtio_device *g_vdev;

static void entry_vq_poll(void)
{
	if (g_vdev)
		vt_poll(g_vdev);   /* unconditional used-ring poll (see vt_poll) */
}

int nkext_init(void)
{
	unsigned char bus, dev, func;
	struct virtio_device *vdev;
	u64 host;
	int ret;

	/* DRM debug categories (CORE|DRIVER|KMS|PRIME|ATOMIC|VBL|STATE|LEASE|DP). Keep this at 0:
	 * every drm_dbg/atomic-state-dump goes through printk -> knx_log -> the graphical console
	 * (fbcon), and under KMS the console mirror-present re-grabs the scanout on each write, so a
	 * chatty commit (SetCrtc) overwrites the very frame a userland compositor (glkms/nwm) just
	 * presented. Raise to 0x1ff only for a bring-up session that reads the serial log, never for
	 * a normal / compositor run. */
	{ extern unsigned long __drm_debug; __drm_debug = 0x0; }

	/* 0) LinuxKPI async workqueues + timers: create the system queues and register the wait-pump
	 * drain hook BEFORE any driver code (INIT_WORK/schedule_work). Runs inline until the scheduler
	 * is up (workers spawn via knx_run_after_scheduler), so the cooperative probe is unaffected. */
	lkpi_wq_init();

	/* 0b) initialize DRM core (chrdev/class + drm_core_init_complete) before any probe */
	__lkpi_modinit_drm_core_init();

	/* 1) register the unmodified driver (stores &virtio_gpu_driver in g_virtio_drv) */
	lkpi_module_init();
	if (!g_virtio_drv || !g_virtio_drv->probe) {
		knx_log("virtio_gpu: driver did not register a probe\n");
		return -1;
	}

	/* 2) find the modern virtio-gpu PCI function and build a virtio_device for it */
	if (!knx_pci_find(VIRTIO_PCI_VENDOR, VIRTIO_GPU_PCI_DEV, &bus, &dev, &func)) {
		knx_log("virtio_gpu: no 1af4:1050 device (run QEMU with -device virtio-gpu-pci)\n");
		return -1;
	}
	vdev = vt_create(bus, dev, func);
	if (!vdev) {
		knx_log("virtio_gpu: vt_create failed\n");
		return -1;
	}
	g_vdev = vdev;
	lkpi_set_fence_poll(entry_vq_poll);

	/* 2b) feature negotiation — normally done by virtio.c's virtio_dev_probe(), which we
	 * bypass. Without this, vdev->features stays 0 and the driver's
	 * virtio_has_feature(VIRTIO_F_VERSION_1) check in virtio_gpu_init() fails (-ENODEV).
	 * Replicate it: features = (driver feature_table & device) + preserved transport bits. */
	{
		u64 dfeat = vdev->config->get_features(vdev);
		u64 want = 0;
		unsigned int i;
		if (!(dfeat & (1ULL << VIRTIO_F_VERSION_1))) {
			knx_log("virtio_gpu: not a virtio-1.0 device\n");
			return -1;
		}
		for (i = 0; i < g_virtio_drv->feature_table_size; i++)
			want |= (1ULL << g_virtio_drv->feature_table[i]);
		vdev->features = want & dfeat;
		/* transport feature bits (incl VIRTIO_F_VERSION_1) are always preserved */
		for (i = VIRTIO_TRANSPORT_F_START; i < VIRTIO_TRANSPORT_F_END; i++)
			if (dfeat & (1ULL << i))
				vdev->features |= (1ULL << i);
		if (vdev->config->finalize_features(vdev) < 0) {
			knx_log("virtio_gpu: finalize_features failed\n");
			return -1;
		}
	}
	host = vdev->features;

	/* Surface the negotiated 3D state as a serial marker the GL smoke gates key on.
	 * VIRTIO_GPU_F_VIRGL is bit 0 (uapi/linux/virtio_gpu.h); define locally to avoid
	 * pulling the full uapi header into this transport-glue TU. */
#ifndef VIRTIO_GPU_F_VIRGL
#define VIRTIO_GPU_F_VIRGL 0
#endif
	knx_log((vdev->features & (1ULL << VIRTIO_GPU_F_VIRGL))
			? "virtio_gpu: virgl 3D negotiated\n"
			: "virtio_gpu: 2D only (no virgl)\n");

	/* 3) hand the device to the UNMODIFIED virtio_gpu_probe() */
	knx_log("virtio_gpu: probing unmodified Linux virtio_gpu DRM driver...\n");
	ret = g_virtio_drv->probe(vdev);
	if (ret) {
		knx_log("virtio_gpu: virtio_gpu_probe() failed\n");
		return -1;
	}
	knx_log("virtio_gpu: virtio_gpu_probe() OK — DRM device up\n");

	/* 4) bring up a scanout via the DRM device and bridge it to /dev/fb0 */
	if (virtio_gpu_fbcon_bringup(vdev) == 0)
		knx_log("virtio_gpu: scanout up; /dev/fb0 bridged via the DRM driver\n");
	else
		knx_log("virtio_gpu: DRM up; scanout/fbcon bring-up pending\n");

	/* 5) expose the real DRM ioctl ABI on /dev/dri/{card0,renderD128}. Register
	 * unconditionally — dumb BOs + KMS are useful even on 2D-only QEMU; virgl-only
	 * ioctls simply return errors there. */
	virtio_gpu_drm_node_init(vdev);

	/* 6) Now that the vqs exist and the device is DRIVER_OK, wire its MSI-X to a shim irq (Task 2).
	 * The used-ring poll stays the harvester; this proves request_irq -> MSI end-to-end on QEMU. */
	vt_enable_msi(vdev);

	return 0;
}
