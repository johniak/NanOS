/*
 * glpix.nxe — the render→scanout oracle. Proves a HOST-GPU-RENDERED virgl resource reaches the
 * physical display, with NO guest CPU readback, over the exact path the GL desktop will use.
 *
 * Why this test exists: gles2info established that Mesa drives virgl end-to-end (renderer=virgl,
 * shaders compile/link/draw with glerr=0) and that guest<->host transfers work BOTH ways
 * (glTexImage2D upload + glReadPixels of an *uploaded* texture are byte-exact). The one gap was
 * reading back a GPU-*rendered* FBO to the guest CPU, which returns zero on this virglrenderer/
 * ANGLE-Metal fork. But glReadPixels-from-a-rendered-FBO is a path the desktop NEVER takes — the
 * desktop renders into a resource and hands it to KMS (set_scanout + resource_flush), and the
 * host resolves the rendered contents to the display itself. This oracle exercises THAT path:
 *
 *   1. open /dev/dri/card0 (primary node: master => KMS ioctls, and virtgpu render ioctls are
 *      DRM_RENDER_ALLOW so they work on the same fd — one fd, one GEM namespace, no PRIME).
 *   2. GETRESOURCES + GETCONNECTOR: pick a connector that advertises a mode.
 *   3. VIRTGPU_RESOURCE_CREATE a 3D render target sized to the mode, bind =
 *      RENDER_TARGET | SCANOUT (so virglrenderer keeps it host-side renderable AND scannable).
 *   4. VIRTGPU_EXECBUFFER a virgl CLEAR-to-magenta stream against it — the host GPU renders.
 *   5. MODE_ADDFB + MODE_SETCRTC that resource's GEM handle: the unmodified virtio_gpu KMS path
 *      issues SET_SCANOUT + RESOURCE_FLUSH, and the host resolves the rendered resource to screen.
 *
 * If the display shows magenta, the render→display path works end to end and the readback quirk
 * is confirmed off the desktop's critical path. Magenta (1,0,1) is chosen because no console/
 * greenter/greeter surface produces it — an unambiguous "the GPU render is on screen" signal.
 *
 * Raw DRM ABI only (no libdrm), against the vendored uapi headers so every struct/ioctl number is
 * byte-identical to the driver — same discipline as drmtest.nxe, whose virgl_words.h we reuse.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/virtgpu_drm.h>
#include "virgl_words.h"      /* build_clear_stream() + VIRGL_FORMAT_/VIRGL_BIND_RENDER_TARGET */

/* Not in virgl_words.h: the scanout bind (virgl_hw.h). A resource that KMS scans out must carry
 * it so virglrenderer keeps a display-resolvable copy. */
#ifndef VIRGL_BIND_SCANOUT
#define VIRGL_BIND_SCANOUT (1u << 18)
#endif

static int die(const char *m) { printf("glpix: FAIL %s\n", m); return 1; }

int main(void)
{
	int fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) return die("open card0");

	/* 0. confirm virgl 3D is available — without it the render step is meaningless. */
	{
		uint64_t has3d = 0;
		struct drm_virtgpu_getparam gp; memset(&gp, 0, sizeof gp);
		gp.param = VIRTGPU_PARAM_3D_FEATURES;
		gp.value = (uintptr_t) &has3d;
		if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) || !has3d)
			return die("no-virgl (need the GL QEMU: -device virtio-vga-gl)");
	}

	/* 1. find a connector that advertises a mode + a crtc to drive it. */
	struct drm_mode_card_res res; memset(&res, 0, sizeof res);
	uint32_t conns[8], crtcs[8], encs[8], fbs[8];
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return die("GETRESOURCES probe");
	if (res.count_connectors > 8 || res.count_crtcs > 8) return die("too many res");
	res.connector_id_ptr = (uintptr_t) conns; res.crtc_id_ptr = (uintptr_t) crtcs;
	res.encoder_id_ptr = (uintptr_t) encs;    res.fb_id_ptr = (uintptr_t) fbs;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return die("GETRESOURCES");
	if (!res.count_connectors || !res.count_crtcs) return die("no connectors/crtcs");

	struct drm_mode_modeinfo modes[32];
	struct drm_mode_modeinfo mode; int have_mode = 0; uint32_t conn_id = 0;
	for (unsigned ci = 0; ci < res.count_connectors && !have_mode; ci++) {
		struct drm_mode_get_connector c; memset(&c, 0, sizeof c);
		c.connector_id = conns[ci];
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
		if (!c.count_modes) continue;
		memset(modes, 0, sizeof modes);
		if (c.count_modes > 32) c.count_modes = 32;
		c.modes_ptr = (uintptr_t) modes; c.count_props = 0; c.count_encoders = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || !c.count_modes) continue;
		mode = modes[0]; conn_id = conns[ci]; have_mode = 1;
	}
	if (!have_mode) return die("no connector modes");
	printf("glpix: mode %ux%u on conn %u\n", mode.hdisplay, mode.vdisplay, conn_id);

	/* 2. host-renderable + scannable 3D render target, sized to the mode. */
	struct drm_virtgpu_resource_create rc; memset(&rc, 0, sizeof rc);
	rc.target = 2;                                   /* PIPE_TEXTURE_2D */
	rc.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
	rc.bind   = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SCANOUT;
	rc.width  = mode.hdisplay; rc.height = mode.vdisplay;
	rc.depth  = 1; rc.array_size = 1;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc)) return die("RESOURCE_CREATE 3D");

	/* 3. the host GPU renders: clear the resource to magenta (1,0,1,1). Passing bo_handle
	 * attaches the resource to this drm_file's virgl context (created lazily on first 3D ioctl),
	 * exactly as drmtest does. */
	uint32_t cmd[64];
	unsigned n = build_clear_stream(cmd, rc.res_handle, rc.format, 1.0f, 0.0f, 1.0f, 1.0f);
	struct drm_virtgpu_execbuffer eb; memset(&eb, 0, sizeof eb);
	eb.command = (uintptr_t) cmd;
	eb.size = n * 4;
	eb.bo_handles = (uintptr_t) &rc.bo_handle;
	eb.num_bo_handles = 1;
	if (ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb)) return die("EXECBUFFER clear");
	printf("glpix: host-render (clear magenta) submitted\n");

	/* 4. scan the RENDERED resource out. ADDFB wraps the GEM bo as a KMS framebuffer; SETCRTC
	 * binds it to the crtc, and the unmodified KMS path issues SET_SCANOUT + RESOURCE_FLUSH so
	 * the host resolves the rendered contents to the display — no guest CPU readback anywhere. */
	struct drm_mode_fb_cmd fb; memset(&fb, 0, sizeof fb);
	fb.width = mode.hdisplay; fb.height = mode.vdisplay; fb.bpp = 32; fb.depth = 24;
	fb.pitch = mode.hdisplay * 4u; fb.handle = rc.bo_handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb)) return die("ADDFB");
	struct drm_mode_crtc sc; memset(&sc, 0, sizeof sc);
	sc.crtc_id = crtcs[0]; sc.fb_id = fb.fb_id; sc.set_connectors_ptr = (uintptr_t) &conn_id;
	sc.count_connectors = 1; sc.mode = mode; sc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) return die("SETCRTC");

	printf("glpix: render-scanout OK\n");   /* smoke gate greps this; screendump captures magenta */
	sleep(3);
	return 0;
}
