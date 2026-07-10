/*
 * i915_drm_node.c — expose the UNMODIFIED i915 driver through the real DRM ioctl ABI
 * (/dev/dri/card0 + /dev/dri/renderD128).
 *
 * Same dispatch shape as kext/virtio_gpu/virtio_gpu_drm_node.c: one drm_file per NanOS process,
 * every ioctl forwarded through the real drm_ioctl() (drm_ioctl.c:762 reads filp->private_data
 * as the drm_file*, copies the arg struct in/out itself, and enforces the render-node subset),
 * a primary-node open becomes DRM master so KMS ioctls (ADDFB/SETCRTC/page-flip) pass the
 * drm_is_current_master() gate and GETCONNECTOR runs the forced fill_modes() probe.
 *
 * The one i915-specific piece is GEM mmap fake-offset resolution: i915 does NOT use the shmem
 * GEM helpers — every mmap offset is a struct i915_mmap_offset (gem/i915_gem_object_types.h:224)
 * whose vma_node lives in dev->vma_offset_manager (mmap_offset_attach, gem/i915_gem_mman.c:717),
 * and the CPU view is the object's own pinned page list (obj->mm.pages). pat_enabled() is true
 * in the shim (io.h:13), so MODE_MAP_DUMB and GEM_MMAP_OFFSET hand out page-backed types
 * (WC/WB/UC) — resolvable to a flat physical range because the shim's shmem provider backs each
 * object with ONE contiguous block (kpi_misc.c shmem_file_setup). I915_MMAP_TYPE_GTT would need
 * a live GGTT binding + the GMADR aperture window; nothing asks for it under pat_enabled, so it
 * fails loud with -ENODEV instead of mapping the wrong thing.
 */
#include "i915_drv.h"
#include "gem/i915_gem_object.h"
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_vma_manager.h>
#include <linux/scatterlist.h>
#include <linux/err.h>
#include <linux/errno.h>
#include "drm_internal.h"       /* drm_file_alloc / drm_file_free (DRM-core-internal) */
#include "../virtio_gpu/knx_drm_node.h"
#include "lkpi_knx.h"

/* DELIBERATE cap, same as virtio_gpu: 64 concurrent GPU clients, loud -ENOMEM when full. */
#define NODE_MAX_CLIENTS 64

static struct drm_device *g_ddev;

struct node_client { int pid; struct drm_file *file; struct file shim; };
static struct node_client g_cli[NODE_MAX_CLIENTS];

/* Suspend/resume the desktop->panel mirror (i915_present.c) while a userland KMS client owns
 * the scanout — when the mirror is armed, its per-frame memcpy would overwrite every frame the
 * client presents. (In the self-map case the mirror is never armed and this is a no-op.) */
void i915_present_set_suspended(int s);

/* Replay the post-probe plane-1A register snapshot (i915_present.c) — used both when the last
 * client goes away (node_release) and on explicit client request (see the private ioctl below). */
int i915_scanout_restore(void);

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
		knx_log("i915: /dev/dri client table full (>64) — rejecting open\n");
		return 0;
	}
	{
		struct drm_minor *minor = (node == KNX_DRM_NODE_RENDER && g_ddev->render)
					      ? g_ddev->render : g_ddev->primary;
		struct drm_file *f = drm_file_alloc(minor);
		if (IS_ERR_OR_NULL(f))
			return 0;
		/* Mirror drm_open_helper(): the first primary-node client must become DRM
		 * master, exactly as in virtio_gpu_drm_node.c (see the rationale there —
		 * without it every DRM_MASTER ioctl returns -EACCES and connector->modes
		 * stays empty). drm_file_free() calls drm_master_release() on teardown. */
		if (drm_is_primary_client(f)) {
			int mret = drm_master_open(f);
			if (mret) {
				knx_log("i915: drm_master_open failed on /dev/dri/card0\n");
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

/* NanOS-private ioctl: replay the boot-scanout snapshot NOW (same i915_scanout_restore() the
 * node_release path uses). Needed because node_release only fires when a process's LAST DRM fd
 * closes — but Mesa (iris) dups the screen fd, so a compositor that tears down its GL/KMS state
 * mid-session (nwm's CPU fallback after a GL failure) keeps the device open: its RmFB made DRM
 * core disable the primary plane, and nothing ever brought the panel back (Dell boot #47: desktop
 * flashed once, then black while the CPU compositor drew into an unscanned fb0). Value must match
 * user/glkms/glkms_init.h (NANOS_DRM_IOCTL_SCANOUT_RESTORE): _IO('d', 0x9f) — the last driver-
 * private command nr, far above anything i915 defines, so it can never shadow a real ioctl. */
#define NANOS_DRM_IOCTL_SCANOUT_RESTORE 0x649f

static long node_ioctl(int pid, int node, unsigned int cmd, void *arg)
{
	struct node_client *c;
	long r;
	void lkpi_tasklet_drain(void);
	/* Cross-core gate FIRST (kpi_misc.c): a DRM ioctl executes the whole i915 stack in this
	 * process's context on whatever core the scheduler picked, under the shim's no-op locks —
	 * it must never run concurrently with the kworker/ktimers/krcu daemons or another client.
	 * Recursive per task, so every wait/pump inside the ioctl re-enters legally. */
	lkpi_gate_enter();
	/* Thread context (per DRM ioctl, ~every frame): (1) drain log lines buffered from IRQ/atomic
	 * context — they cannot append to the USB-backed log there without re-entering g_xhciLock and
	 * deadlocking (see kpi_print.c tee ring); (2) run any execlists tasklet the GT harvest
	 * deferred (P6/Task 3), so submission makes progress even if this frame doesn't wait. */
	lkpi_log_flush();
	lkpi_tasklet_drain();
	if (!g_ddev) {
		lkpi_gate_exit();
		return -ENODEV;
	}
	if (cmd == NANOS_DRM_IOCTL_SCANOUT_RESTORE) {
		int rr = i915_scanout_restore();
		(void)arg;
		if (rr == 0)
			knx_log("i915: scanout restored on client request — boot fb scanning again\n");
		else
			knx_log("i915: client scanout-restore FAILED (no snapshot or transcoder off)\n");
		i915_present_set_suspended(0);
		lkpi_gate_exit();
		return rr == 0 ? 0 : -EIO;
	}
	/* Re-base the stack-overflow tripwire onto THIS ioctl's per-task 128 KiB heap stack (the probe
	 * baseline was the loader's 1 MiB stack, far below — so lkpi_stack_deep was inert at runtime). Now
	 * a runaway execbuf -> inline execlists tasklet -> dma_fence_signal callback chain that descends
	 * past the runtime redline names its call site (ra) via the FS-teed log BEFORE the scheduler's
	 * guard band trips. */
	lkpi_stack_baseline();
	lkpi_set_current_client(pid);   /* drm_ioctl logs current->comm/pid — name the real client */
	c = client_get(pid, node);
	if (!c) {
		lkpi_gate_exit();
		return -ENOMEM;
	}
	r = drm_ioctl(&c->shim, cmd, (unsigned long)arg);
	/* A successful SETCRTC means a KMS client now owns the scanout — stop the mirror
	 * (node_release resumes it when the client goes away). */
	if (r == 0 && cmd == DRM_IOCTL_MODE_SETCRTC)
		i915_present_set_suspended(1);
	lkpi_gate_exit();
	return r;
}

/* Resolve a GEM mmap fake-offset (bytes, from MODE_MAP_DUMB / GEM_MMAP_OFFSET) to the physical
 * range of the object's backing pages. The DRM vma manager keys nodes by page start. */
static int node_mmap_offset_gated(uint64_t off, uint64_t *phys, uint64_t *len);

static int node_mmap_offset(int pid, uint64_t off, uint64_t *phys, uint64_t *len)
{
	int r;
	(void)pid;
	if (!g_ddev)
		return -ENODEV;
	/* Same one-executor rule as node_ioctl: the vma lookup + pin walk driver state. */
	lkpi_gate_enter();
	r = node_mmap_offset_gated(off, phys, len);
	lkpi_gate_exit();
	return r;
}

static int node_mmap_offset_gated(uint64_t off, uint64_t *phys, uint64_t *len)
{
	struct drm_vma_offset_node *vnode;
	struct i915_mmap_offset *mmo;
	struct drm_i915_gem_object *obj;
	struct scatterlist *sg;
	unsigned long long p0 = 0, expect = 0;
	int r;

	drm_vma_offset_lock_lookup(g_ddev->vma_offset_manager);
	vnode = drm_vma_offset_exact_lookup_locked(g_ddev->vma_offset_manager,
						   off >> PAGE_SHIFT, 1);
	drm_vma_offset_unlock_lookup(g_ddev->vma_offset_manager);
	if (!vnode)
		return -EINVAL;
	mmo = container_of(vnode, struct i915_mmap_offset, vma_node);
	obj = mmo->obj;

	if (mmo->mmap_type == I915_MMAP_TYPE_GTT) {
		knx_log("i915: GTT-type mmap offset unsupported (pat_enabled shim hands out WC) — refusing mmap\n");
		return -ENODEV;
	}

	/* Pin the backing pages; never unpinned while mapped, matching the no-reclaim KPI
	 * (same lifetime rule as the virtio node's drm_gem_shmem_pin). */
	r = i915_gem_object_pin_pages_unlocked(obj);
	if (r)
		return r;
	if (!obj->mm.pages || !obj->mm.pages->sgl)
		return -ENOMEM;

	/* The shim's shmem provider allocates one contiguous block, so the sg list must
	 * describe one flat physical range. Assert it (fail loud, never map a corrupt range). */
	for (sg = obj->mm.pages->sgl; sg; sg = sg_next(sg)) {
		unsigned long long p = (unsigned long long)sg_phys(sg);
		if (!p0)
			p0 = expect = p;
		if (p != expect) {
			knx_log("i915: GEM object not physically contiguous — refusing mmap\n");
			return -EIO;
		}
		expect += sg->length;
	}
	if (!p0)
		return -ENOMEM;
	*phys = p0;
	*len  = obj->base.size;
	return 0;
}

static void node_release(int pid)
{
	int i, left = 0, freed = 0;
	lkpi_gate_enter();              /* drm_file_free tears down GEM/KMS state — one-executor rule */
	lkpi_set_current_client(pid);   /* drm_file_free logs current->comm too */
	for (i = 0; i < NODE_MAX_CLIENTS; i++) {
		if (g_cli[i].file && g_cli[i].pid == pid) {
			drm_file_free(g_cli[i].file);
			g_cli[i].file = 0;
			g_cli[i].pid = 0;
			freed = 1;
		} else if (g_cli[i].file)
			left++;
	}
	if (!freed || left) {
		lkpi_gate_exit();
		return;
	}
	/* Last KMS client gone. drm_file_free -> drm_fb_release -> atomic_remove_fb disabled the
	 * primary plane (transcoder stays up), blanking the panel. Bring the console back by
	 * replaying the plane registers snapshotted after probe, then resume the mirror (coarse
	 * single-compositor model, same as virtio). */
	{
		int r = i915_scanout_restore();
		if (r == 0)
			knx_log("i915: scanout restored after last KMS client — console visible again\n");
		else if (r == -2)
			knx_log("i915: scanout restore skipped — transcoder off (full modeset needed, reboot)\n");
	}
	i915_present_set_suspended(0);
	lkpi_gate_exit();
}

static const struct knx_drm_ops g_node_ops = { node_ioctl, node_mmap_offset, node_release };

int i915_drm_node_init(struct pci_dev *pdev)
{
	/* i915_driver_create: pci_set_drvdata(pdev, &i915->drm) — drvdata IS the drm_device. */
	struct drm_device *ddev = (struct drm_device *)pci_get_drvdata(pdev);
	if (!ddev || !ddev->primary)
		return -1;
	g_ddev = ddev;
	knx_drm_register(&g_node_ops);
	return 0;
}
