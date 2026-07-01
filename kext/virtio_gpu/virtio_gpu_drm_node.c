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
		g_cli[free_i].pid = pid;
		g_cli[free_i].file = f;
		g_cli[free_i].shim.private_data = f;   /* what drm_ioctl() reads */
		return &g_cli[free_i];
	}
}

static long node_ioctl(int pid, int node, unsigned int cmd, void *arg)
{
	struct node_client *c;
	if (!g_ddev)
		return -ENODEV;
	c = client_get(pid, node);
	if (!c)
		return -ENOMEM;
	return drm_ioctl(&c->shim, cmd, (unsigned long)arg);
}

static int node_mmap_offset(int pid, uint64_t off, uint64_t *phys, uint64_t *len)
{
	(void)pid; (void)off; (void)phys; (void)len;
	return -ENOSYS;   /* Task 5 fills this in (GEM fake-offset -> physical range) */
}

static void node_release(int pid)
{
	int i;
	for (i = 0; i < NODE_MAX_CLIENTS; i++)
		if (g_cli[i].file && g_cli[i].pid == pid) {
			drm_file_free(g_cli[i].file);
			g_cli[i].file = 0;
			g_cli[i].pid = 0;
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
