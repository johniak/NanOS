/*
 * glkms_init.c — GBM + EGL + KMS present path (see glkms_init.h).
 *
 * Driver-load path (same as gles2info's fd-direct GBM route): we open the DRM node ourselves and
 * hand its fd to gbm_create_device, then eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm). This
 * avoids Mesa's surfaceless device enumeration (which walks sysfs — NanOS has none) and drives
 * loader_get_driver_for_fd → drmGetVersion → "virtio_gpu" → the statically-linked gallium-virgl
 * megadriver. Unlike gles2info we use the PRIMARY node (card0) because we need KMS master to
 * modeset, and we create a real gbm_surface (SCANOUT|RENDERING) rather than surfaceless.
 */
#include "glkms_init.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>    /* malloc/free: the full EGL config list is driver-sized, not a fixed 32 */
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define GLKMS_FORMAT GBM_FORMAT_XRGB8888

/* Diagnostic print hook (glkms_init.h): default = plain printf, so nwm's GL backend behaves as
 * before; the glkms oracle repoints it at its tee so stage verdicts reach gltest.txt. */
static int glkms_printf_default(const char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vprintf(fmt, ap);
	va_end(ap);
	return r;
}
int (*glkms_printf)(const char *fmt, ...) = glkms_printf_default;
#define printf glkms_printf

/* KMS discovery: pick a connector that advertises a mode, and a crtc that can drive it. Mirrors
 * glpix's raw probe but via libdrm's drmMode* wrappers. */
static int kms_pick(struct glkms *g)
{
	drmModeRes *res = drmModeGetResources(g->fd);
	if (!res) { printf("glkms: drmModeGetResources failed (%d)\n", errno); return -1; }

	int found = 0;
	for (int i = 0; i < res->count_connectors && !found; i++) {
		drmModeConnector *c = drmModeGetConnector(g->fd, res->connectors[i]);
		if (!c) continue;
		if (c->count_modes > 0) {
			uint32_t crtc = 0;
			/* prefer the connector's currently-bound encoder+crtc */
			if (c->encoder_id) {
				drmModeEncoder *e = drmModeGetEncoder(g->fd, c->encoder_id);
				if (e) { crtc = e->crtc_id; drmModeFreeEncoder(e); }
			}
			/* else walk this connector's encoders and take the first possible crtc */
			for (int ei = 0; ei < c->count_encoders && !crtc; ei++) {
				drmModeEncoder *e = drmModeGetEncoder(g->fd, c->encoders[ei]);
				if (!e) continue;
				for (int ci = 0; ci < res->count_crtcs; ci++)
					if (e->possible_crtcs & (1u << ci)) { crtc = res->crtcs[ci]; break; }
				drmModeFreeEncoder(e);
			}
			if (!crtc && res->count_crtcs) crtc = res->crtcs[0];
			if (crtc) {
				g->conn_id = c->connector_id;
				g->crtc_id = crtc;
				g->mode    = c->modes[0];
				g->mode_w  = c->modes[0].hdisplay;
				g->mode_h  = c->modes[0].vdisplay;
				found = 1;
			}
		}
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	if (!found) { printf("glkms: no connector with a mode + crtc\n"); return -1; }
	return 0;
}

/* Choose an ES2 window config whose native visual id matches our GBM format (required so the EGL
 * window surface and the gbm_surface agree on pixel layout).
 *
 * Scan EVERY matching config, not a 32-slot prefix: EGL sorts deeper color buffers first, and a
 * hardware driver's list is huge (iris: 16F/2101010 x MSAA x depth variants), so the 24-bit
 * XRGB8888 entry lands far past any small prefix (Dell boot #44: the old cfgs[0] fallback picked
 * an incompatible deep format -> eglCreateWindowSurface EGL_BAD_MATCH). ARGB8888 is accepted as
 * second choice — Mesa's dri2_drm_config_is_compatible explicitly allows ARGB<->XRGB mixing on a
 * GBM surface. Anything else can only fail surface creation, so fail HERE, loudly, instead. */
static int egl_pick_config(struct glkms *g)
{
	static const EGLint attr[] = {
		EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE,   8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE,  8,
		EGL_ALPHA_SIZE, 0,
		EGL_NONE
	};
	static const EGLint want[] = { (EGLint)GLKMS_FORMAT, (EGLint)GBM_FORMAT_ARGB8888 };
	EGLint total = 0;
	if (!eglChooseConfig(g->dpy, attr, 0, 0, &total) || total <= 0) {
		printf("glkms: eglChooseConfig failed n=%d (0x%x)\n", total, eglGetError());
		return -1;
	}
	EGLConfig *cfgs = malloc(sizeof(EGLConfig) * total);
	if (!cfgs) {
		printf("glkms: config list alloc failed (n=%d)\n", total);
		return -1;
	}
	EGLint n = 0;
	if (!eglChooseConfig(g->dpy, attr, cfgs, total, &n) || n <= 0) {
		printf("glkms: eglChooseConfig failed n=%d (0x%x)\n", n, eglGetError());
		free(cfgs);
		return -1;
	}
	for (unsigned w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
		for (int i = 0; i < n; i++) {
			EGLint vid = 0;
			if (eglGetConfigAttrib(g->dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid) &&
			    vid == want[w]) {
				g->cfg = cfgs[i];
				free(cfgs);
				return 0;
			}
		}
	}
	/* Neither XRGB8888 nor ARGB8888: dump what the driver DOES offer so the pulled log names
	 * the candidates (fourccs) instead of a bare failure. */
	printf("glkms: no [AX]RGB8888 config among %d; visuals:", n);
	for (int i = 0; i < n && i < 12; i++) {
		EGLint vid = 0;
		eglGetConfigAttrib(g->dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid);
		printf(" 0x%x", vid);
	}
	printf("%s\n", n > 12 ? " ..." : "");
	free(cfgs);
	return -1;
}

int glkms_open(struct glkms *g)
{
	memset(g, 0, sizeof *g);
	g->fd = -1;

	g->fd = open("/dev/dri/card0", O_RDWR);
	if (g->fd < 0) { printf("glkms: open card0 failed (%d)\n", errno); goto fail; }

	if (kms_pick(g) != 0) goto fail;
	printf("glkms: mode %ux%u crtc=%u conn=%u\n", g->mode_w, g->mode_h, g->crtc_id, g->conn_id);

	g->gbm = gbm_create_device(g->fd);
	if (!g->gbm) { printf("glkms: gbm_create_device failed\n"); goto fail; }

	g->surf = gbm_surface_create(g->gbm, g->mode_w, g->mode_h, GLKMS_FORMAT,
	                             GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!g->surf) { printf("glkms: gbm_surface_create failed\n"); goto fail; }

	g->dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, g->gbm, 0);
	if (g->dpy == EGL_NO_DISPLAY) { printf("glkms: eglGetPlatformDisplay failed\n"); goto fail; }
	if (!eglInitialize(g->dpy, 0, 0)) { printf("glkms: eglInitialize failed (0x%x)\n", eglGetError()); goto fail; }
	if (!eglBindAPI(EGL_OPENGL_ES_API)) { printf("glkms: eglBindAPI failed (0x%x)\n", eglGetError()); goto fail; }

	if (egl_pick_config(g) != 0) goto fail;

	g->esurf = eglCreateWindowSurface(g->dpy, g->cfg, (EGLNativeWindowType)g->surf, 0);
	if (g->esurf == EGL_NO_SURFACE) { printf("glkms: eglCreateWindowSurface failed (0x%x)\n", eglGetError()); goto fail; }

	{
		static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
		g->ctx = eglCreateContext(g->dpy, g->cfg, EGL_NO_CONTEXT, ctx_attr);
	}
	if (g->ctx == EGL_NO_CONTEXT) { printf("glkms: eglCreateContext failed (0x%x)\n", eglGetError()); goto fail; }
	if (!eglMakeCurrent(g->dpy, g->esurf, g->esurf, g->ctx)) {
		printf("glkms: eglMakeCurrent failed (0x%x)\n", eglGetError());
		goto fail;
	}
	return 0;

fail:
	glkms_close(g);
	return -1;
}

int glkms_swap(struct glkms *g)
{
	if (!eglSwapBuffers(g->dpy, g->esurf)) {
		printf("glkms: eglSwapBuffers failed (0x%x)\n", eglGetError());
		return -1;
	}
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(g->surf);
	if (!bo) { printf("glkms: gbm_surface_lock_front_buffer failed\n"); return -1; }

	uint32_t handle = gbm_bo_get_handle(bo).u32;
	uint32_t stride = gbm_bo_get_stride(bo);
	uint32_t fb = 0;
	/* legacy AddFB (depth 24 / bpp 32 → XRGB8888) — the path glpix's raw MODE_ADDFB proved on the
	 * virtio_gpu KMS driver. */
	if (drmModeAddFB(g->fd, g->mode_w, g->mode_h, 24, 32, stride, handle, &fb)) {
		printf("glkms: drmModeAddFB failed (%d)\n", errno);
		gbm_surface_release_buffer(g->surf, bo);
		return -1;
	}
	/* No page-flip events yet (DrmDevice::read is the follow-on) → SetCrtc every swap. Tearing is
	 * accepted at bring-up. */
	if (drmModeSetCrtc(g->fd, g->crtc_id, fb, 0, 0, &g->conn_id, 1, &g->mode)) {
		printf("glkms: drmModeSetCrtc failed (%d)\n", errno);
		drmModeRmFB(g->fd, fb);
		gbm_surface_release_buffer(g->surf, bo);
		return -1;
	}

	/* retire the previous front buffer now that the new one owns the scanout */
	if (g->front) {
		drmModeRmFB(g->fd, g->front_fb);
		gbm_surface_release_buffer(g->surf, g->front);
	}
	g->front    = bo;
	g->front_fb = fb;
	g->crtc_set = 1;
	return 0;
}

void glkms_close(struct glkms *g)
{
	/* Stage markers: a mid-session teardown (nwm's GL->CPU fallback) has HUNG inside this
	 * function (QEMU repro of Dell boot #49: the loop never came back; last output was the
	 * fallback line). Each stage prints BEFORE it runs so the pulled log names the hang. */
	printf("glkms: close: egl teardown\n");
	if (g->dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (g->ctx   != EGL_NO_CONTEXT) eglDestroyContext(g->dpy, g->ctx);
		if (g->esurf != EGL_NO_SURFACE) eglDestroySurface(g->dpy, g->esurf);
		eglTerminate(g->dpy);
	}
	printf("glkms: close: retire front fb\n");
	if (g->front) {
		drmModeRmFB(g->fd, g->front_fb);
		gbm_surface_release_buffer(g->surf, g->front);
		g->front = 0;
	}
	printf("glkms: close: gbm teardown\n");
	if (g->surf) gbm_surface_destroy(g->surf);
	if (g->gbm)  gbm_device_destroy(g->gbm);
	/* We owned the scanout (crtc_set) and just removed its framebuffer, which disabled the
	 * primary plane. Hand the panel back to the boot fb NOW — see the define in glkms_init.h
	 * for why waiting for the fd-close replay is not enough. Best-effort by design. */
	if (g->crtc_set && g->fd >= 0) {
		printf("glkms: close: scanout restore\n");
		drmIoctl(g->fd, NANOS_DRM_IOCTL_SCANOUT_RESTORE, 0);
	}
	if (g->fd >= 0) close(g->fd);
	printf("glkms: close: done\n");
	memset(g, 0, sizeof *g);
	g->fd = -1;
	g->dpy = EGL_NO_DISPLAY;
}

/* See glkms_init.h: failure-path teardown that cannot block — scanout back to the boot fb,
 * close our fd, LEAK the (possibly wedged) EGL/GBM state on purpose. */
void glkms_close_wedged(struct glkms *g)
{
	if (g->crtc_set && g->fd >= 0) {
		printf("glkms: wedged close: scanout restore\n");
		drmIoctl(g->fd, NANOS_DRM_IOCTL_SCANOUT_RESTORE, 0);
	}
	if (g->fd >= 0) close(g->fd);
	printf("glkms: wedged close: done (EGL/GBM state leaked by design)\n");
	memset(g, 0, sizeof *g);
	g->fd = -1;
	g->dpy = EGL_NO_DISPLAY;
}
