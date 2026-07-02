/*
 * virtio_gpu_drm_node.c — expose the UNMODIFIED virtio_gpu driver through the real DRM ioctl ABI.
 *
 * Dispatch prerequisites (verified against the vendored 6.12 source):
 *   - long drm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)   [drm_ioctl.c:762]
 *       reads filp->private_data as the struct drm_file*, copies the arg struct in/out itself
 *       (copy_{from,to}_user are direct memcpy in linuxkpi — user memory is mapped during a
 *       syscall), and enforces the render-node subset via drm_file->minor.
 *   - struct drm_file *drm_file_alloc(struct drm_minor *minor)                  [drm_file.c:130]
 *   - void            drm_file_free(struct drm_file *file)                      [drm_file.c:223]
 *   - the drm_device (vdev->priv, as in virtio_gpu_present.c) carries ->primary and ->render
 *     minors; the driver advertises DRIVER_RENDER, so drm_dev_register() creates both.
 *
 * One drm_file per NanOS process (apps open a node once, so per-open == per-process here);
 * dispatch through drm_ioctl() so DRM core + virtgpu ioctls all work with zero re-implementation.
 */
#include <linux/virtio.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vma_manager.h>
#include <linux/mm.h>       /* page_to_phys, PAGE_SHIFT/PAGE_SIZE */
#include "drm_internal.h"   /* drm_file_alloc / drm_file_free (DRM-core-internal) */
#include "knx_drm_node.h"
#include "lkpi_knx.h"

/* DELIBERATE cap: 64 concurrent GPU clients is ample for this system (far below the kernel's
 * ProcTable::MAX = 1024). The table fails loud (-ENOMEM + knx_log) when full. */
#define NODE_MAX_CLIENTS 64

static struct drm_device *g_ddev;

struct node_client { int pid; struct drm_file *file; struct file shim; };
static struct node_client g_cli[NODE_MAX_CLIENTS];

/* Find (or lazily create) the drm_file for `pid` on the given node kind. */
static struct node_client *client_get(int pid, int node)
{
	int free_i = -1;
	int i;
	for (i = 0; i < NODE_MAX_CLIENTS; i++) {
		if (g_cli[i].file && g_cli[i].pid == pid)
			return &g_cli[i];
		if (!g_cli[i].file && free_i < 0)
			free_i = i;
	}
	if (free_i < 0) {
		knx_log("virtio_gpu: /dev/dri client table full (>64) — rejecting open\n");
		return 0;
	}
	{
		struct drm_minor *minor = (node == KNX_DRM_NODE_RENDER && g_ddev->render)
					      ? g_ddev->render : g_ddev->primary;
		struct drm_file *f = drm_file_alloc(minor);
		if (IS_ERR_OR_NULL(f))
			return 0;
		/* Mirror drm_open_helper(): a primary-node open must become DRM master when none
		 * exists. Without this drm_is_current_master() is false, so drm_mode_getconnector()
		 * SKIPS the forced fill_modes() probe (connector->modes stays empty -> count_modes=0)
		 * and every DRM_MASTER ioctl (SETCRTC/ADDFB/page-flip) returns -EACCES. The kernel
		 * mirror-fb present path never runs a KMS probe, so this forced probe is the ONLY thing
		 * that populates connector->modes for userland KMS clients (glkms/glpix/drmtest).
		 * drm_file_free() (node_release) already calls drm_master_release() for primary clients. */
		if (drm_is_primary_client(f)) {
			int mret = drm_master_open(f);
			if (mret) {
				knx_log("virtio_gpu: drm_master_open failed on /dev/dri/card0\n");
				drm_file_free(f);
				return 0;
			}
		}
		g_cli[free_i].pid = pid;
		g_cli[free_i].file = f;
		g_cli[free_i].shim.private_data = f;   /* what drm_ioctl() reads */
		return &g_cli[free_i];
	}
}

/* Suspend the kernel console/fb0 mirror-present while a userland client drives the CRTC via KMS
 * (defined in virtio_gpu_present.c). Without this, the periodic console mirror re-flushes the
 * fbcon resource onto scanout 0 and overwrites every frame glkms/nwm present. */
void virtio_gpu_present_set_suspended(int s);

static long node_ioctl(int pid, int node, unsigned int cmd, void *arg)
{
	struct node_client *c;
	long r;
	if (!g_ddev)
		return -ENODEV;
	c = client_get(pid, node);
	if (!c)
		return -ENOMEM;
	r = drm_ioctl(&c->shim, cmd, (unsigned long)arg);
	/* A successful MODE_SETCRTC means a KMS client now owns the scanout — stop the console mirror.
	 * (node_release re-enables it when the client goes away.) */
	if (r == 0 && cmd == DRM_IOCTL_MODE_SETCRTC)
		virtio_gpu_present_set_suspended(1);
	return r;
}

/* Resolve a GEM mmap fake-offset (bytes, as handed to userspace by e.g. MODE_MAP_DUMB /
 * VIRTGPU_MAP) to the physical range of the backing shmem object. The DRM vma manager keys
 * nodes by page start (off >> PAGE_SHIFT). */
static int node_mmap_offset(int pid, uint64_t off, uint64_t *phys, uint64_t *len)
{
	struct drm_vma_offset_node *vnode;
	struct drm_gem_object *obj;
	struct drm_gem_shmem_object *shmem;
	unsigned long p0;
	unsigned long npages, i;
	(void)pid;
	if (!g_ddev)
		return -ENODEV;

	drm_vma_offset_lock_lookup(g_ddev->vma_offset_manager);
	vnode = drm_vma_offset_exact_lookup_locked(g_ddev->vma_offset_manager,
						   off >> PAGE_SHIFT, 1);
	drm_vma_offset_unlock_lookup(g_ddev->vma_offset_manager);
	if (!vnode)
		return -EINVAL;
	obj = container_of(vnode, struct drm_gem_object, vma_node);

	/* Pin the shmem pages (drm_gem_shmem_get_pages is static in 6.12; drm_gem_shmem_pin is the
	 * exported entry point — it populates shmem->pages under dma_resv and pins them; we never
	 * unpin while mapped, matching our no-reclaim KPI). */
	shmem = to_drm_gem_shmem_obj(obj);
	if (!shmem->pages) {
		int r = drm_gem_shmem_pin(shmem);
		if (r)
			return r;
	}
	if (!shmem->pages || !shmem->pages[0])
		return -ENOMEM;

	/* Our shmem backing (linuxkpi kpi_mm) is one contiguous block: phys of page 0 covers the
	 * whole object. Assert contiguity (fail loud, never map a corrupt range). */
	p0 = (unsigned long) page_to_phys(shmem->pages[0]);
	npages = (obj->size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	for (i = 1; i < npages; i++) {
		if (!shmem->pages[i] ||
		    (unsigned long) page_to_phys(shmem->pages[i]) != p0 + i * PAGE_SIZE) {
			knx_log("virtio_gpu: GEM shmem not physically contiguous — refusing mmap\n");
			return -EIO;
		}
	}
	*phys = p0;
	*len  = obj->size;
	return 0;
}

static void node_release(int pid)
{
	int i;
	for (i = 0; i < NODE_MAX_CLIENTS; i++)
		if (g_cli[i].file && g_cli[i].pid == pid) {
			drm_file_free(g_cli[i].file);
			g_cli[i].file = 0;
			g_cli[i].pid = 0;
			/* the KMS client is gone — hand the scanout back to the console mirror. (Coarse: any
			 * client release resumes it. A second live KMS client would re-suspend on its next
			 * SETCRTC; fine for the single-compositor model here.) */
			virtio_gpu_present_set_suspended(0);
		}
}

static const struct knx_drm_ops g_node_ops = { node_ioctl, node_mmap_offset, node_release };

int virtio_gpu_drm_node_init(struct virtio_device *vdev)
{
	struct drm_device *ddev = (struct drm_device *)vdev->priv;
	if (!ddev)
		return -1;
	g_ddev = ddev;
	knx_drm_register(&g_node_ops);
	knx_log("virtio_gpu: /dev/dri nodes registered\n");
	return 0;
}
