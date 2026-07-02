/*
 * virtio_transport.c — modern virtio-pci transport for the NanOS LinuxKPI module.
 *
 * Implements virtio_config_ops over the lifted vp_modern_* accessors (config space, BARs,
 * queue programming) + virtio_ring (vring_create_virtqueue). One INTx-style interrupt is
 * serviced by vt_interrupt() walking the queues; MSI-X is not used.
 */
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_pci_modern.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/cache.h>
#include "lkpi_knx.h"
#include "virtio_transport.h"

#define VT_MAX_VQ 8

struct vt_dev {
	struct virtio_device vdev;
	struct virtio_pci_modern_device mdev;
	struct pci_dev pdev;
	struct virtqueue *vqs[VT_MAX_VQ];
	void __iomem *notify[VT_MAX_VQ];
	unsigned nvqs;
};

static struct vt_dev *to_vt(struct virtio_device *vdev) {
	return container_of(vdev, struct vt_dev, vdev);
}

/* --- config_ops --- */

static void vt_get(struct virtio_device *vdev, unsigned offset, void *buf, unsigned len) {
	struct vt_dev *vt = to_vt(vdev);
	u8 *p = (u8 *)buf;
	u8 __iomem *src = (u8 __iomem *)vt->mdev.device + offset;
	for (unsigned i = 0; i < len; i++)
		p[i] = readb(src + i);
}

static void vt_set(struct virtio_device *vdev, unsigned offset, const void *buf, unsigned len) {
	struct vt_dev *vt = to_vt(vdev);
	const u8 *p = (const u8 *)buf;
	u8 __iomem *dst = (u8 __iomem *)vt->mdev.device + offset;
	for (unsigned i = 0; i < len; i++)
		writeb(p[i], dst + i);
}

static u32 vt_generation(struct virtio_device *vdev) { return vp_modern_generation(&to_vt(vdev)->mdev); }
static u8  vt_get_status(struct virtio_device *vdev) { return vp_modern_get_status(&to_vt(vdev)->mdev); }
static void vt_set_status(struct virtio_device *vdev, u8 status) { vp_modern_set_status(&to_vt(vdev)->mdev, status); }

static void vt_reset(struct virtio_device *vdev) {
	struct vt_dev *vt = to_vt(vdev);
	unsigned long guard = 100000000UL;
	vp_modern_set_status(&vt->mdev, 0);
	/* modern spec: wait until the device clears status to 0 */
	while (vp_modern_get_status(&vt->mdev) && --guard)
		__asm__ __volatile__("pause");
	if (!guard) knx_log("virtio_transport: DIAG vt_reset status never cleared\n");
}

static u64 vt_get_features(struct virtio_device *vdev) { return vp_modern_get_features(&to_vt(vdev)->mdev); }

static int vt_finalize_features(struct virtio_device *vdev) {
	struct vt_dev *vt = to_vt(vdev);
	vring_transport_features(vdev);
	if (!__virtio_test_bit(vdev, VIRTIO_F_VERSION_1)) {
		knx_log("virtio_transport: device lacks VIRTIO_F_VERSION_1\n");
		return -ENODEV;
	}
	vp_modern_set_features(&vt->mdev, vdev->features);
	return 0;
}

static const char *vt_bus_name(struct virtio_device *vdev) { (void)vdev; return "virtio-pci-modern"; }

static bool vt_vq_notify(struct virtqueue *vq) {
	struct vt_dev *vt = to_vt(vq->vdev);
	writew((u16)vq->index, vt->notify[vq->index]);
	return true;
}

static int vt_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
                       struct virtqueue *vqs[], struct virtqueue_info vqs_info[],
                       struct irq_affinity *desc) {
	struct vt_dev *vt = to_vt(vdev);
	(void)desc;
	if (nvqs > VT_MAX_VQ)
		return -EINVAL;
	for (unsigned i = 0; i < nvqs; i++) {
		struct virtqueue_info *vqi = &vqs_info[i];
		if (!vqi->name) { vqs[i] = 0; vt->vqs[i] = 0; continue; }

		u16 num = vp_modern_get_queue_size(&vt->mdev, i);
		if (!num)
			return -ENOENT;

		struct virtqueue *vq = vring_create_virtqueue(i, num, SMP_CACHE_BYTES, vdev,
		                                              true, true, vqi->ctx,
		                                              vt_vq_notify, vqi->callback, vqi->name);
		if (!vq)
			return -ENOMEM;

		vp_modern_set_queue_size(&vt->mdev, i, virtqueue_get_vring_size(vq));
		vp_modern_queue_address(&vt->mdev, i,
		                        virtqueue_get_desc_addr(vq),
		                        virtqueue_get_avail_addr(vq),
		                        virtqueue_get_used_addr(vq));
		vt->notify[i] = vp_modern_map_vq_notify(&vt->mdev, i, 0);
		vp_modern_set_queue_enable(&vt->mdev, i, true);
		vt->vqs[i] = vq;
		vqs[i] = vq;
	}
	vt->nvqs = nvqs;
	return 0;
}

static void vt_del_vqs(struct virtio_device *vdev) {
	struct vt_dev *vt = to_vt(vdev);
	for (unsigned i = 0; i < vt->nvqs; i++) {
		if (vt->vqs[i]) {
			vp_modern_set_queue_enable(&vt->mdev, i, false);
			vring_del_virtqueue(vt->vqs[i]);
			vt->vqs[i] = 0;
		}
	}
	vt->nvqs = 0;
}

static const struct virtio_config_ops vt_config_ops = {
	.get               = vt_get,
	.set               = vt_set,
	.generation        = vt_generation,
	.get_status        = vt_get_status,
	.set_status        = vt_set_status,
	.reset             = vt_reset,
	.find_vqs          = vt_find_vqs,
	.del_vqs           = vt_del_vqs,
	.get_features      = vt_get_features,
	.finalize_features = vt_finalize_features,
	.bus_name          = vt_bus_name,
};

void vt_interrupt(struct virtio_device *vdev) {
	struct vt_dev *vt = to_vt(vdev);
	u8 isr = readb(vt->mdev.isr);
	if (!isr)
		return;
	for (unsigned i = 0; i < vt->nvqs; i++)
		if (vt->vqs[i])
			vring_interrupt(0, vt->vqs[i]);
}

/* Cooperative poll used by the wait-pump (lkpi_wait_pump -> entry_vq_poll). Unlike vt_interrupt,
 * this does NOT gate on the read-to-clear ISR register: in a busy-poll a used-buffer completion
 * can land in the window after the poll already cleared ISR, so the completion would never be
 * harvested and the waiter (e.g. a full ctrl vq or a fence) would hang forever. Walking the used
 * rings unconditionally is the correct polling model — vring_interrupt/virtqueue_get_buf simply
 * find nothing when there is nothing new. (Read ISR too, to keep the level-triggered line clear.) */
void vt_poll(struct virtio_device *vdev) {
	struct vt_dev *vt = to_vt(vdev);
	(void) readb(vt->mdev.isr);   /* clear the level-triggered ISR; result ignored */
	for (unsigned i = 0; i < vt->nvqs; i++)
		if (vt->vqs[i])
			vring_interrupt(0, vt->vqs[i]);
}

/* Normally in virtio.c (the bus layer we don't lift): a debug check that a driver only
 * uses features it declared in its id_table. We bind the device directly, so it's a no-op. */
void virtio_check_driver_offered_feature(const struct virtio_device *vdev, unsigned int fbit) {
	(void)vdev; (void)fbit;
}

struct virtio_device *vt_create(unsigned char bus, unsigned char dev, unsigned char func) {
	struct vt_dev *vt = (struct vt_dev *)kzalloc(sizeof(*vt), GFP_KERNEL);
	if (!vt)
		return 0;

	vt->pdev.nbus = bus; vt->pdev.ndev = dev; vt->pdev.nfunc = func;
	lkpi_pci_fill_ids(&vt->pdev);
	pci_enable_device(&vt->pdev);   /* enable MEM decode + bus master before BAR mapping */

	/* Cooperative bring-up: we never wire the device's INTx line — used-buffer completions
	 * are harvested by polling the ISR register via the wait-pump (lkpi_wait_pump -> vt_interrupt).
	 * virtio INTx is LEVEL-triggered, so once QEMU asserts it on the first kick and nobody acks
	 * the PCI interrupt, the line stays high and the CPU storms the (unhandled) vector forever,
	 * stalling pre-scheduler boot. Set PCI_COMMAND.INTX_DISABLE (bit 10) so the pin never asserts;
	 * the ISR status register still reflects queue completions for the cooperative poll. */
	{
		unsigned int cmd = knx_pci_cfg_read32(bus, dev, func, 0x04);
		knx_pci_cfg_write32(bus, dev, func, 0x04, cmd | (1u << 10));
	}

	vt->mdev.pci_dev = &vt->pdev;
	if (vp_modern_probe(&vt->mdev) < 0) {
		knx_log("virtio_transport: vp_modern_probe failed\n");
		kfree(vt);
		return 0;
	}

	vt->vdev.dev.parent = &vt->pdev.dev;
	vt->vdev.config = &vt_config_ops;
	vt->vdev.id.device = vt->mdev.id.device;
	vt->vdev.id.vendor = vt->mdev.id.vendor;
	vt->vdev.priv = vt;
	INIT_LIST_HEAD(&vt->vdev.vqs);
	spin_lock_init(&vt->vdev.config_lock);
	spin_lock_init(&vt->vdev.vqs_list_lock);

	/* bring the device to ACKNOWLEDGE | DRIVER (the driver probe does the rest) */
	vp_modern_set_status(&vt->mdev, 0);
	{
		unsigned long guard = 100000000UL;
		while (vp_modern_get_status(&vt->mdev) && --guard)
			__asm__ __volatile__("pause");
		if (!guard) knx_log("virtio_transport: DIAG vt_create status never cleared\n");
	}
	vp_modern_set_status(&vt->mdev, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

	return &vt->vdev;
}
