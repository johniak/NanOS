/*
 * virtio_gpu_kext.c — the NanOS LinuxKPI module entry (nkext_init).
 *
 * P1.3 milestone: find the modern virtio-gpu PCI function, build a virtio_device via the
 * hand-built transport, negotiate VIRTIO_F_VERSION_1, set up the control virtqueue, and
 * exchange a VIRTIO_GPU_CMD_GET_DISPLAY_INFO with the device — proving the whole virtio
 * path (transport + vring + DMA) end to end. P2 replaces this with the vendored virtio_gpu
 * DRM driver probe; the transport stays.
 */
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <uapi/linux/virtio_gpu.h>
#include "lkpi_knx.h"
#include "virtio_transport.h"

/* modern virtio PCI id: 0x1AF4:0x1040+devtype; virtio-gpu is device type 16 -> 0x1050 */
#define VIRTIO_PCI_VENDOR  0x1AF4
#define VIRTIO_GPU_PCI_DEV 0x1050

static void uitoa(char *b, unsigned v) {
	char t[12]; int n = 0;
	if (!v) { b[0] = '0'; b[1] = 0; return; }
	while (v) { t[n++] = '0' + (v % 10); v /= 10; }
	int i = 0; while (n) b[i++] = t[--n]; b[i] = 0;
}
static void logn(const char *pfx, unsigned w, unsigned h) {
	char line[96], num[12]; int i = 0;
	for (const char *p = pfx; *p; p++) line[i++] = *p;
	uitoa(num, w); for (char *p = num; *p; p++) line[i++] = *p;
	line[i++] = 'x';
	uitoa(num, h); for (char *p = num; *p; p++) line[i++] = *p;
	line[i++] = '\n'; line[i] = 0;
	knx_log(line);
}

int nkext_init(void) {
	unsigned char bus, dev, func;
	if (!knx_pci_find(VIRTIO_PCI_VENDOR, VIRTIO_GPU_PCI_DEV, &bus, &dev, &func)) {
		knx_log("virtio_gpu: no 1af4:1050 device (run QEMU with -device virtio-gpu-pci)\n");
		return 0;   /* not an error: just no device on this machine */
	}
	knx_log("virtio_gpu: found modern virtio-gpu PCI function\n");

	struct virtio_device *vdev = vt_create(bus, dev, func);
	if (!vdev) {
		knx_log("virtio_gpu: vt_create failed\n");
		return -1;
	}

	/* feature negotiation: we require VIRTIO_F_VERSION_1 */
	u64 host = vdev->config->get_features(vdev);
	if (!(host & (1ULL << VIRTIO_F_VERSION_1))) {
		knx_log("virtio_gpu: device is not virtio-1.0\n");
		return -1;
	}
	vdev->features = (1ULL << VIRTIO_F_VERSION_1);
	if (vdev->config->finalize_features(vdev) < 0) {
		knx_log("virtio_gpu: finalize_features failed\n");
		return -1;
	}
	u8 st = vdev->config->get_status(vdev);
	vdev->config->set_status(vdev, st | VIRTIO_CONFIG_S_FEATURES_OK);
	st = vdev->config->get_status(vdev);
	if (!(st & VIRTIO_CONFIG_S_FEATURES_OK)) {
		knx_log("virtio_gpu: device rejected FEATURES_OK\n");
		return -1;
	}

	/* set up controlq (0) + cursorq (1) */
	struct virtqueue *vqs[2];
	struct virtqueue_info vqi[2] = {
		{ "control", 0, false },
		{ "cursor",  0, false },
	};
	if (vdev->config->find_vqs(vdev, 2, vqs, vqi, 0) < 0) {
		knx_log("virtio_gpu: find_vqs failed\n");
		return -1;
	}
	struct virtqueue *controlq = vqs[0];

	st = vdev->config->get_status(vdev);
	vdev->config->set_status(vdev, st | VIRTIO_CONFIG_S_DRIVER_OK);
	knx_log("virtio_gpu: DRIVER_OK; controlq up\n");

	/* exchange GET_DISPLAY_INFO on the control queue */
	struct virtio_gpu_ctrl_hdr *cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	struct virtio_gpu_resp_display_info *resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!cmd || !resp) { knx_log("virtio_gpu: oom\n"); return -1; }
	cmd->type = cpu_to_le32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);

	struct scatterlist sg_out, sg_in;
	sg_init_one(&sg_out, cmd, sizeof(*cmd));
	sg_init_one(&sg_in, resp, sizeof(*resp));
	struct scatterlist *sgs[2] = { &sg_out, &sg_in };

	if (virtqueue_add_sgs(controlq, sgs, 1, 1, cmd, GFP_KERNEL) < 0) {
		knx_log("virtio_gpu: virtqueue_add_sgs failed\n");
		return -1;
	}
	virtqueue_kick(controlq);

	unsigned int len = 0;
	void *done = 0;
	for (int i = 0; i < 20000000 && !done; i++) {
		done = virtqueue_get_buf(controlq, &len);
		if (!done) __asm__ __volatile__("pause");
	}
	if (!done) {
		knx_log("virtio_gpu: GET_DISPLAY_INFO timed out\n");
		return -1;
	}

	if (le32_to_cpu(resp->hdr.type) != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
		knx_log("virtio_gpu: unexpected display-info response\n");
		return -1;
	}
	unsigned w = le32_to_cpu(resp->pmodes[0].r.width);
	unsigned h = le32_to_cpu(resp->pmodes[0].r.height);
	logn("virtio_gpu: scanout0 ", w, h);
	knx_log("virtio_gpu: GET_DISPLAY_INFO OK (P1 checkpoint)\n");
	return 0;
}
