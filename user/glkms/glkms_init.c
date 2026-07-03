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
 * window surface and the gbm_surface agree on pixel layout). */
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
	EGLConfig cfgs[32];
	EGLint n = 0;
	if (!eglChooseConfig(g->dpy, attr, cfgs, 32, &n) || n <= 0) {
		printf("glkms: eglChooseConfig failed n=%d (0x%x)\n", n, eglGetError());
		return -1;
	}
	for (int i = 0; i < n; i++) {
		EGLint vid = 0;
		if (eglGetConfigAttrib(g->dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid) &&
		    vid == (EGLint)GLKMS_FORMAT) {
			g->cfg = cfgs[i];
			return 0;
		}
	}
	/* No exact native-visual match: fall back to the first config. XRGB8888 is what virgl
	 * exposes, so this path is a safety net, not the expected one. */
	g->cfg = cfgs[0];
	return 0;
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
	if (g->dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (g->ctx   != EGL_NO_CONTEXT) eglDestroyContext(g->dpy, g->ctx);
		if (g->esurf != EGL_NO_SURFACE) eglDestroySurface(g->dpy, g->esurf);
		eglTerminate(g->dpy);
	}
	if (g->front) {
		drmModeRmFB(g->fd, g->front_fb);
		gbm_surface_release_buffer(g->surf, g->front);
		g->front = 0;
	}
	if (g->surf) gbm_surface_destroy(g->surf);
	if (g->gbm)  gbm_device_destroy(g->gbm);
	if (g->fd >= 0) close(g->fd);
	memset(g, 0, sizeof *g);
	g->fd = -1;
	g->dpy = EGL_NO_DISPLAY;
}
