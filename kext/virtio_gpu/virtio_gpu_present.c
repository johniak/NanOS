/*
 * virtio_gpu_present.c — scanout bring-up + /dev/fb0 bridge over the UNMODIFIED driver.
 *
 * After virtio_gpu_drv_entry.c runs the real virtio_gpu_probe() (DRM device, GEM, KMS all up),
 * this turns the DRM device into a usable system framebuffer THROUGH the unmodified driver's own
 * command layer (virtgpu_vq.c) — no hand-rolled virtio-gpu protocol:
 *
 *   1. virtio_gpu_object_create()  — the driver allocates a shmem GEM bo and issues the real
 *      RESOURCE_CREATE_2D + RESOURCE_ATTACH_BACKING for scanout 0's framebuffer.
 *   2. virtio_gpu_cmd_set_scanout() — bind that resource to scanout 0.
 *   3. knx_fb_set_backing()        — expose the bo's (contiguous) backing as /dev/fb0 so the VT
 *      console + nwm draw straight into it.
 *   4. present_flush() (the periodic present cb) issues the driver's TRANSFER_TO_HOST_2D +
 *      RESOURCE_FLUSH each frame, so whatever was drawn is scanned out by the device.
 *
 * The shmem backing is one physically-contiguous block (see linuxkpi/kpi_misc.c), so
 * page_address(bo->base.pages[0]) is a flat framebuffer the CPU and the device share.
 */
#include "virtgpu_drv.h"
#include "lkpi_knx.h"

extern void knx_fb_set_backing(unsigned long long phys, unsigned int pitch,
			       unsigned int w, unsigned int h, unsigned char bpp,
			       void (*flush)(void));
extern void lkpi_wait_pump(void);

static struct virtio_gpu_device *g_vgdev;
static struct virtio_gpu_object  *g_bo;
static u32 g_w, g_h, g_resid;

/* Console/fb0 mirror suspend. While a userland DRM client drives the CRTC via KMS (glkms / the nwm
 * GL compositor issue their own SET_SCANOUT for the frame they rendered), the periodic console
 * mirror below must NOT re-flush the fbcon resource onto scanout 0 — otherwise it fights the KMS
 * client and the console overwrites every presented frame. Mirrors real Linux suspending fbcon
 * when a DRM master takes over the CRTC. Set by the DRM node on a successful MODE_SETCRTC and
 * cleared when the client releases the node (virtio_gpu_drm_node.c). */
static volatile int g_present_suspended;
void virtio_gpu_present_set_suspended(int s) { g_present_suspended = s ? 1 : 0; }

/* Periodic present: hand the freshly-drawn framebuffer to the host and flush scanout 0.
 * Runs on the kernel present thread (post-scheduler); the pump harvests the vq acks
 * cooperatively (INTx is masked — see virtio_transport.c). */
static void present_flush(void)
{
	struct virtio_gpu_object_array *objs;

	if (!g_vgdev || !g_bo)
		return;
	if (g_present_suspended)   /* a KMS client owns the CRTC — don't fight its scanout */
		return;

	objs = virtio_gpu_array_alloc(1);
	if (!objs)
		return;
	virtio_gpu_array_add_obj(objs, &g_bo->base.base);

	virtio_gpu_cmd_transfer_to_host_2d(g_vgdev, 0, g_w, g_h, 0, 0, objs, NULL);
	virtio_gpu_cmd_resource_flush(g_vgdev, g_resid, 0, 0, g_w, g_h, NULL, NULL);
	virtio_gpu_notify(g_vgdev);
	lkpi_wait_pump();
}

int virtio_gpu_fbcon_bringup(struct virtio_device *vdev)
{
	struct drm_device *ddev = (struct drm_device *)vdev->priv;
	struct virtio_gpu_device *vgdev;
	struct virtio_gpu_object_params params;
	struct virtio_gpu_object *bo = NULL;
	unsigned long long phys;
	u32 w, h;
	int ret;

	if (!ddev)
		return -1;
	vgdev = ddev->dev_private;
	if (!vgdev || !vgdev->num_scanouts)
		return -1;

	w = le32_to_cpu(vgdev->outputs[0].info.r.width);
	h = le32_to_cpu(vgdev->outputs[0].info.r.height);
	if (!w || !h) { w = 1280; h = 800; }

	/* 1) let the driver create scanout 0's framebuffer resource + backing */
	memset(&params, 0, sizeof(params));
	params.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
	params.width  = w;
	params.height = h;
	params.size   = (unsigned long)w * h * 4u;
	params.dumb   = true;

	ret = virtio_gpu_object_create(vgdev, &params, &bo, NULL);
	if (ret || !bo) {
		knx_log("virtio_gpu: scanout object_create failed\n");
		return -1;
	}
	virtio_gpu_notify(vgdev);
	lkpi_wait_pump();   /* CREATE_2D + ATTACH_BACKING acks */

	g_vgdev = vgdev;
	g_bo    = bo;
	g_w     = w;
	g_h     = h;
	g_resid = bo->hw_res_handle;

	/* 2) bind the resource to scanout 0 */
	virtio_gpu_cmd_set_scanout(vgdev, 0, bo->hw_res_handle, w, h, 0, 0);
	virtio_gpu_notify(vgdev);
	lkpi_wait_pump();

	/* 3) the contiguous shmem backing IS the system framebuffer (identity-mapped:
	 * the page's kernel virtual address equals its physical address). */
	phys = (unsigned long long)(unsigned long)bo->base.pages[0];

	present_flush();   /* push the initial (cleared) frame */

	/* 4) expose /dev/fb0 over it + register the periodic present callback */
	knx_fb_set_backing(phys, w * 4u, w, h, 32, present_flush);
	knx_log("virtio_gpu: scanout 0 up via the unmodified DRM driver; /dev/fb0 bridged\n");
	return 0;
}
