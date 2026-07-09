/*
 * glkms_init.h — GBM + EGL + KMS present path shared between the glkms oracle (glkms.c) and the
 * nwm GL compositor backend (Task 10, nw_compose_gl.c). This is the canonical Linux GPU-present
 * sequence: a GBM scanout surface on /dev/dri/card0, an EGL context on the GBM platform, and
 * eglSwapBuffers → gbm_surface_lock_front_buffer → drmModeAddFB → drmModeSetCrtc. The host GPU
 * (virglrenderer→ANGLE→Metal on QEMU; i915 on the Dell) resolves the rendered front buffer to the
 * scanout — no guest CPU readback, which is exactly why this works where glReadPixels does not.
 *
 * All functions return 0 on success, -1 on failure (state is cleaned up on failure so the caller
 * can fall back to the CPU compositor). Depends on libdrm (drmMode*), libgbm, libEGL — built via
 * the Mesa/libdrm ports (link closure in mesa-port/build-glkms.sh).
 */
#pragma once
#include <stdint.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <xf86drmMode.h>

struct glkms {
	int                 fd;        /* /dev/dri/card0 (primary node → KMS master) */
	struct gbm_device  *gbm;
	struct gbm_surface *surf;
	EGLDisplay          dpy;
	EGLContext          ctx;
	EGLConfig           cfg;
	EGLSurface          esurf;
	uint32_t            crtc_id;
	uint32_t            conn_id;
	uint32_t            mode_w, mode_h;
	drmModeModeInfo     mode;      /* the mode we drive the crtc with */
	uint32_t            saved_crtc_fb; /* to (best-effort) not leave a dangling crtc on close */
	struct gbm_bo      *front;     /* the bo currently on scanout */
	uint32_t            front_fb;  /* its KMS framebuffer id */
	int                 crtc_set;  /* first swap does SetCrtc, later swaps re-SetCrtc (no flip evt yet) */
};

/* NanOS-private DRM ioctl (i915 node only): replay the boot-scanout plane registers immediately.
 * glkms_close() issues it after retiring its framebuffers — removing the fb that was live scanout
 * makes DRM core DISABLE the primary plane, and the kext's node_release replay never fires while
 * Mesa's dup'd screen fd keeps the device open in a still-running process (Dell boot #47: nwm's
 * GL→CPU fallback left the panel black while the CPU compositor drew into an unscanned fb0).
 * Value = _IO('d', 0x9f); must match kext/i915/i915_drm_node.c. Best-effort: other drivers
 * (virtio_gpu on QEMU) reject the unknown command and the caller ignores the result. */
#define NANOS_DRM_IOCTL_SCANOUT_RESTORE 0x649f

/* Print hook for every glkms_init diagnostic line (stage failures, mode line). Defaults to plain
 * printf (console) so nwm's GL backend is unchanged; the glkms oracle points it at its tee_printf
 * so stage verdicts land in /nanos/logs/gltest.txt — the only channel `make i915-log` can read
 * (Dell boot #43 failed inside glkms_open and the stage marker was console-only, i.e. lost). */
extern int (*glkms_printf)(const char *fmt, ...);

/* LOSSLESS DIAG sink: write() one formatted line straight to /disks/main/nanos/logs/gldiag.txt,
 * bypassing the buffered printf->pipe->logger->nwm.txt path that loses its tail on a Dell reboot.
 * Used by the GL-freeze bring-up markers (glkms_swap, nw_gl_frame). Remove with them. */
void glkms_diag(const char *fmt, ...);

/* Full init: open card0, GBM device + scanout surface at the connector's preferred mode, EGL GBM
 * display + ES2 context, make current. On success the caller may issue GL and call glkms_swap. */
int  glkms_open(struct glkms *g);

/* Present the current GL frame: eglSwapBuffers, lock the new front buffer, wrap it as a KMS fb,
 * and scan it out (SetCrtc). Retires the previous front buffer. Tearing is accepted at bring-up
 * (page-flip events via DrmDevice::read are the recorded follow-on). */
int  glkms_swap(struct glkms *g);

/* Release EGL/GBM/KMS state. Safe to call on a partially-initialised struct (glkms_open cleans up
 * itself on failure, but glkms_close is idempotent for the success path teardown). */
void glkms_close(struct glkms *g);

/* WEDGED teardown — for the mid-session failure path (nwm's GL->CPU fallback) ONLY. A context
 * that just failed a frame may have unsignalled fences, and the graceful teardown (eglMakeCurrent/
 * eglTerminate/gbm destroy) waits on them WITHOUT timeout — that hang froze the desktop after the
 * fallback (Dell boot #49, reproduced on QEMU: the event loop never returned from teardown). This
 * variant touches NOTHING that can block: restore the boot scanout (register replay) and close our
 * fd, deliberately LEAKING the EGL/GBM objects — the process keeps running, the kernel reclaims
 * everything at process exit. Graceful glkms_close remains for healthy teardowns. */
void glkms_close_wedged(struct glkms *g);
