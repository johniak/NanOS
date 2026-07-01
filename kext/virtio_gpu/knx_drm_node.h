/*
 * knx_drm_node.h — the op table the virtio_gpu kext registers with the kernel so /dev/dri/card0
 * and /dev/dri/renderD128 (kernel-side DrmDevice char devices) can forward SYS_ioctl / SYS_mmap
 * into the vendored DRM stack living inside virtio_gpu.nkext.
 *
 * Plain C, NO Linux headers — included by both the kext (C) and the kernel (C++), so it must not
 * pull in any DRM/linux type. The kext side implements the ops over drm_ioctl()/drm_file; the
 * kernel side (DrmDevice) only routes to them.
 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Node kinds — which DRM node a call came in on. Render nodes only accept the render-allowed
 * subset (DRM core enforces this via drm_file.minor). */
#define KNX_DRM_NODE_PRIMARY 0   /* /dev/dri/card0     */
#define KNX_DRM_NODE_RENDER  1   /* /dev/dri/renderD128 */

struct knx_drm_ops {
    /* Forward one ioctl. pid identifies the calling process (per-process drm_file),
     * node is KNX_DRM_NODE_*, cmd is the FULL Linux ioctl number (incl. size/dir bits),
     * arg is the user pointer as passed to SYS_ioctl. Returns 0/-errno. */
    long (*ioctl)(int pid, int node, unsigned int cmd, void *arg);
    /* Resolve a GEM mmap fake-offset (as returned by the driver in e.g.
     * DRM_IOCTL_VIRTGPU_MAP / mode_create_dumb) to a physical range for this pid.
     * Returns 0 and fills phys/len, or -errno. */
    int  (*mmap_offset)(int pid, uint64_t offset, uint64_t *phys_out, uint64_t *len_out);
    /* Process teardown: release the pid's drm_file + all its GEM handles. */
    void (*release)(int pid);
};

#ifdef __cplusplus
}
#endif
