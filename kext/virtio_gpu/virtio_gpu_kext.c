/*
 * virtio_gpu_kext.c — the NanOS LinuxKPI virtio-gpu module entry (nkext_init).
 *
 * Built on the hand-built modern virtio-pci transport (virtio_transport.c) over the LIFTED,
 * UNMODIFIED Linux virtio core (virtio_ring.c + virtio_pci_modern_dev.c) bound through the
 * LinuxKPI shim. The display is driven via the virtio-gpu device protocol (the vendored
 * Linux uapi structs): GET_DISPLAY_INFO -> RESOURCE_CREATE_2D -> ATTACH_BACKING ->
 * SET_SCANOUT -> TRANSFER_TO_HOST_2D -> RESOURCE_FLUSH.
 *
 * (The full Linux virtio_gpu DRM/KMS driver lift remains a documented follow-on; the DRM
 * core is ~tens of thousands of LOC. This path delivers a working scanout on the same
 * lifted-virtio-core foundation and is the basis for the /dev/fb0 bridge in P3.)
 */
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <uapi/linux/virtio_gpu.h>
#include "lkpi_knx.h"
#include "virtio_transport.h"

#define VIRTIO_PCI_VENDOR  0x1AF4
#define VIRTIO_GPU_PCI_DEV 0x1050
#define GPU_RESOURCE_ID    1

/* module state (single GPU, single scanout) — kept for the P3 /dev/fb0 bridge */
static struct virtqueue *g_controlq;
static u32 *g_fb;            /* guest framebuffer (BGRX), identity-mapped */
static u32  g_w, g_h;        /* scanout dimensions */
static u32 *g_mirror_src;    /* if set: copy this (the kernel/boot fb) into g_fb each present */
static u32  g_mirror_pitch;  /* source stride in bytes */

/* ---- small decimal logger ---- */
static void log_dim(const char *pfx, unsigned w, unsigned h) {
	char b[96], t[12]; int i = 0, n;
	for (const char *p = pfx; *p; p++) b[i++] = *p;
	if (!w) b[i++] = '0'; else { n = 0; for (unsigned v = w; v; v /= 10) t[n++] = '0' + v % 10; while (n) b[i++] = t[--n]; }
	b[i++] = 'x';
	if (!h) b[i++] = '0'; else { n = 0; for (unsigned v = h; v; v /= 10) t[n++] = '0' + v % 10; while (n) b[i++] = t[--n]; }
	b[i++] = '\n'; b[i] = 0; knx_log(b);
}

/* ---- control-queue command: send `cmd` (clen bytes), read response into `resp` ---- */
static int gpu_cmd(const void *cmd, unsigned clen, void *resp, unsigned rlen) {
	struct scatterlist sg_out, sg_in;
	struct scatterlist *sgs[2];
	sg_init_one(&sg_out, cmd, clen);
	sg_init_one(&sg_in, resp, rlen);
	sgs[0] = &sg_out; sgs[1] = &sg_in;
	if (virtqueue_add_sgs(g_controlq, sgs, 1, 1, (void *)cmd, GFP_KERNEL) < 0)
		return -1;
	virtqueue_kick(g_controlq);
	unsigned int len = 0; void *done = 0;
	for (int i = 0; i < 50000000 && !done; i++) {
		done = virtqueue_get_buf(g_controlq, &len);
		if (!done) __asm__ __volatile__("pause");
	}
	return done ? 0 : -1;
}

/* fill a recognisable SMPTE-ish color-bar test pattern (BGRX: u32 = R<<16|G<<8|B) */
static void draw_test_pattern(void) {
	static const u32 bars[8] = {
		0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
		0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
	};
	for (u32 y = 0; y < g_h; y++)
		for (u32 x = 0; x < g_w; x++)
			g_fb[y * g_w + x] = bars[(x * 8) / g_w];
}

/* transfer the framebuffer to the host resource and flush a rectangle to the scanout */
int virtio_gpu_flush(u32 x, u32 y, u32 w, u32 h) {
	(void)x; (void)y; (void)w; (void)h;   /* flush the whole scanout for simplicity */
	struct virtio_gpu_transfer_to_host_2d *xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	struct virtio_gpu_resource_flush *flush = kzalloc(sizeof(*flush), GFP_KERNEL);
	struct virtio_gpu_ctrl_hdr *resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!xfer || !flush || !resp) return -1;

	xfer->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
	xfer->r.x = 0; xfer->r.y = 0;
	xfer->r.width = cpu_to_le32(g_w); xfer->r.height = cpu_to_le32(g_h);
	xfer->offset = 0;
	xfer->resource_id = cpu_to_le32(GPU_RESOURCE_ID);
	gpu_cmd(xfer, sizeof(*xfer), resp, sizeof(*resp));

	flush->hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
	flush->r.x = 0; flush->r.y = 0;
	flush->r.width = cpu_to_le32(g_w); flush->r.height = cpu_to_le32(g_h);
	flush->resource_id = cpu_to_le32(GPU_RESOURCE_ID);
	gpu_cmd(flush, sizeof(*flush), resp, sizeof(*resp));

	kfree(xfer); kfree(flush); kfree(resp);
	return 0;
}

/* present the whole framebuffer (called periodically by the kernel present thread).
 * In mirror mode, first copy the kernel/boot framebuffer into our DMA-able g_fb. */
void virtio_gpu_present(void) {
	static int once = 0;
	if (!g_fb)
		return;
	if (g_mirror_src) {
		u32 spl = g_mirror_pitch / 4;   /* source pixels per line (pitch may exceed width) */
		for (u32 y = 0; y < g_h; y++)
			memcpy(&g_fb[y * g_w], &g_mirror_src[y * spl], (size_t)g_w * 4);
	}
	if (!once) {
		once = 1;
		char b[80]; int i = 0; const char *m = "virtio_gpu: present#1 src0=0x";
		for (const char *p = m; *p; p++) b[i++] = *p;
		const char *hx = "0123456789abcdef";
		u32 v = g_mirror_src ? g_mirror_src[0] : 0xDEAD;
		for (int s = 28; s >= 0; s -= 4) b[i++] = hx[(v >> s) & 0xf];
		b[i++] = '\n'; b[i] = 0; knx_log(b);
	}
	virtio_gpu_flush(0, 0, g_w, g_h);
}

static int gpu_setup_scanout(void) {
	struct virtio_gpu_ctrl_hdr *resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) return -1;

	/* 1) create a 2D host resource for scanout 0 */
	struct virtio_gpu_resource_create_2d *c2d = kzalloc(sizeof(*c2d), GFP_KERNEL);
	c2d->hdr.type   = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
	c2d->resource_id = cpu_to_le32(GPU_RESOURCE_ID);
	c2d->format     = cpu_to_le32(VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
	c2d->width      = cpu_to_le32(g_w);
	c2d->height     = cpu_to_le32(g_h);
	if (gpu_cmd(c2d, sizeof(*c2d), resp, sizeof(*resp)) < 0) { knx_log("virtio_gpu: CREATE_2D failed\n"); return -1; }

	/* 2) allocate the guest framebuffer and attach it as the resource's backing */
	g_fb = (u32 *)alloc_pages_exact((size_t)g_w * g_h * 4, __GFP_ZERO);
	if (!g_fb) { knx_log("virtio_gpu: fb alloc failed\n"); return -1; }

	struct {
		struct virtio_gpu_resource_attach_backing ab;
		struct virtio_gpu_mem_entry ent;
	} *att = kzalloc(sizeof(*att), GFP_KERNEL);
	att->ab.hdr.type    = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
	att->ab.resource_id = cpu_to_le32(GPU_RESOURCE_ID);
	att->ab.nr_entries  = cpu_to_le32(1);
	att->ent.addr       = cpu_to_le64((u64)(unsigned long)g_fb);   /* identity map: virt == phys */
	att->ent.length     = cpu_to_le32((u32)((size_t)g_w * g_h * 4));
	if (gpu_cmd(att, sizeof(*att), resp, sizeof(*resp)) < 0) { knx_log("virtio_gpu: ATTACH_BACKING failed\n"); return -1; }

	/* 3) bind the resource to scanout 0 */
	struct virtio_gpu_set_scanout *ss = kzalloc(sizeof(*ss), GFP_KERNEL);
	ss->hdr.type     = cpu_to_le32(VIRTIO_GPU_CMD_SET_SCANOUT);
	ss->r.width      = cpu_to_le32(g_w);
	ss->r.height     = cpu_to_le32(g_h);
	ss->scanout_id   = 0;
	ss->resource_id  = cpu_to_le32(GPU_RESOURCE_ID);
	if (gpu_cmd(ss, sizeof(*ss), resp, sizeof(*resp)) < 0) { knx_log("virtio_gpu: SET_SCANOUT failed\n"); return -1; }

	/* 4) draw a test pattern and push it to the display */
	draw_test_pattern();
	virtio_gpu_flush(0, 0, g_w, g_h);

	kfree(c2d); kfree(att); kfree(ss); kfree(resp);
	knx_log("virtio_gpu: scanout configured, test pattern flushed (P2 checkpoint)\n");

	/* 5) P3 — present the NanOS desktop through virtio-gpu. */
	if (g_mirror_src) {
		/* Mirror mode: the kernel already owns /dev/fb0 + VTs on the boot framebuffer and the
		 * proven graphics stack (greeter/nwm/VT-switch) runs there. We just copy that fb onto
		 * the virtio-gpu scanout every frame, so the desktop is displayed BY the Linux virtio
		 * GPU driver stack. */
		knx_fb_start_present(virtio_gpu_present);
		knx_log("virtio_gpu: mirroring the kernel framebuffer -> virtio-gpu (P3 checkpoint)\n");
	} else {
		/* No boot framebuffer: make our buffer THE system fb (creates /dev/fb0 + VT console),
		 * so init brings the desktop up directly on virtio-gpu. */
		knx_fb_set_backing((unsigned long long)(unsigned long)g_fb, g_w * 4, g_w, g_h, 32,
		                   virtio_gpu_present);
		knx_log("virtio_gpu: /dev/fb0 backed by virtio-gpu; present thread up (P3 checkpoint)\n");
	}
	return 0;
}

int nkext_init(void) {
	unsigned char bus, dev, func;
	if (!knx_pci_find(VIRTIO_PCI_VENDOR, VIRTIO_GPU_PCI_DEV, &bus, &dev, &func)) {
		knx_log("virtio_gpu: no 1af4:1050 device (run QEMU with -device virtio-gpu-pci)\n");
		return 0;
	}
	knx_log("virtio_gpu: found modern virtio-gpu PCI function\n");

	struct virtio_device *vdev = vt_create(bus, dev, func);
	if (!vdev) { knx_log("virtio_gpu: vt_create failed\n"); return -1; }

	u64 host = vdev->config->get_features(vdev);
	if (!(host & (1ULL << VIRTIO_F_VERSION_1))) { knx_log("virtio_gpu: not virtio-1.0\n"); return -1; }
	vdev->features = (1ULL << VIRTIO_F_VERSION_1);
	if (vdev->config->finalize_features(vdev) < 0) { knx_log("virtio_gpu: finalize_features failed\n"); return -1; }
	u8 st = vdev->config->get_status(vdev);
	vdev->config->set_status(vdev, st | VIRTIO_CONFIG_S_FEATURES_OK);
	if (!(vdev->config->get_status(vdev) & VIRTIO_CONFIG_S_FEATURES_OK)) { knx_log("virtio_gpu: FEATURES_OK rejected\n"); return -1; }

	struct virtqueue *vqs[2];
	struct virtqueue_info vqi[2] = { { "control", 0, false }, { "cursor", 0, false } };
	if (vdev->config->find_vqs(vdev, 2, vqs, vqi, 0) < 0) { knx_log("virtio_gpu: find_vqs failed\n"); return -1; }
	g_controlq = vqs[0];

	st = vdev->config->get_status(vdev);
	vdev->config->set_status(vdev, st | VIRTIO_CONFIG_S_DRIVER_OK);
	knx_log("virtio_gpu: DRIVER_OK; controlq up\n");

	/* GET_DISPLAY_INFO -> scanout dimensions */
	struct virtio_gpu_ctrl_hdr *cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	struct virtio_gpu_resp_display_info *resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!cmd || !resp) { knx_log("virtio_gpu: oom\n"); return -1; }
	cmd->type = cpu_to_le32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
	if (gpu_cmd(cmd, sizeof(*cmd), resp, sizeof(*resp)) < 0) { knx_log("virtio_gpu: GET_DISPLAY_INFO timed out\n"); return -1; }
	if (le32_to_cpu(resp->hdr.type) != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) { knx_log("virtio_gpu: bad display-info resp\n"); return -1; }

	g_w = le32_to_cpu(resp->pmodes[0].r.width);
	g_h = le32_to_cpu(resp->pmodes[0].r.height);
	if (!g_w || !g_h) { g_w = 1024; g_h = 768; }   /* fallback if scanout 0 is disabled */
	log_dim("virtio_gpu: scanout0 ", g_w, g_h);

	/* If the bootloader gave the kernel a framebuffer, the full graphics stack (VTs, greeter,
	 * nwm, VT switching) is already running on it — mirror it onto virtio-gpu at its exact size
	 * so the real desktop is displayed by the Linux virtio GPU stack. */
	{
		unsigned long long baddr; unsigned int bpitch, bw, bh; unsigned char bbpp;
		if (knx_boot_fb(&baddr, &bpitch, &bw, &bh, &bbpp) && bw && bh) {
			g_w = bw; g_h = bh;
			g_mirror_src = (u32 *)(unsigned long)baddr;
			g_mirror_pitch = bpitch;
			log_dim("virtio_gpu: mirror boot fb ", g_w, g_h);
		}
	}

	kfree(cmd); kfree(resp);
	return gpu_setup_scanout();
}
