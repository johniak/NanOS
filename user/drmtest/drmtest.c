/*
 * drmtest.nxe — raw DRM ABI oracle (no libdrm). Proves the whole kernel DRM surface with plain
 * ioctls before any big port:
 *   Part 1 (card0, works on 2D QEMU): VERSION, GETRESOURCES, GETCONNECTOR, dumb BO create/map/
 *           write, AddFB + SetCrtc — the screen turns solid blue (KMS took scanout from fbcon).
 *   Part 2 (renderD128, needs virgl): GETPARAM 3D, RESOURCE_CREATE 3D, EXECBUFFER clear — proves
 *           the 3D submit path reaches host virglrenderer.
 * Compiled against the vendored uapi headers (-Iexternal/linux-6.12/include/uapi + include), so
 * every struct/ioctl number is byte-identical to the driver's.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/virtgpu_drm.h>
#include "virgl_words.h"

static int die(const char *m){ printf("drmtest: FAIL %s\n", m); return 1; }

static int virgl_part(void)
{
	int rfd = open("/dev/dri/renderD128", O_RDWR);
	if (rfd < 0) return die("open renderD128");

	uint64_t has3d = 0;
	struct drm_virtgpu_getparam gp; memset(&gp, 0, sizeof gp);
	gp.param = VIRTGPU_PARAM_3D_FEATURES;
	gp.value = (uintptr_t) &has3d;
	if (ioctl(rfd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) || !has3d) {
		printf("drmtest: no-virgl (skip 3D)\n");
		return 0;
	}

	/* 3D render-target resource (the driver creates the drm_file's context lazily on the first
	 * 3D ioctl in 6.12; EXECBUFFER triggers it). */
	struct drm_virtgpu_resource_create rc; memset(&rc, 0, sizeof rc);
	rc.target = 2;                              /* PIPE_TEXTURE_2D */
	rc.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
	rc.bind   = VIRGL_BIND_RENDER_TARGET;
	rc.width = 640; rc.height = 480; rc.depth = 1; rc.array_size = 1;
	if (ioctl(rfd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc)) return die("RESOURCE_CREATE 3D");

	uint32_t cmd[64];
	unsigned n = build_clear_stream(cmd, rc.res_handle, rc.format,
					0.0f, 1.0f, 0.0f, 1.0f);   /* clear to green */
	struct drm_virtgpu_execbuffer eb; memset(&eb, 0, sizeof eb);
	eb.command = (uintptr_t) cmd;
	eb.size = n * 4;
	eb.bo_handles = (uintptr_t) &rc.bo_handle;
	eb.num_bo_handles = 1;
	if (ioctl(rfd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb)) return die("EXECBUFFER clear");
	printf("drmtest: virgl-clear OK\n");
	return 0;
}

int main(void)
{
	int fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) return die("open card0");

	{ /* 1. version */
		char name[64] = {0};
		struct drm_version v; memset(&v, 0, sizeof v);
		v.name = name; v.name_len = sizeof name - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION, &v)) return die("VERSION");
		printf("drmtest: version=%s\n", name);          /* expect virtio_gpu */
	}

	struct drm_mode_card_res res; memset(&res, 0, sizeof res);
	uint32_t conns[8], crtcs[8], encs[8], fbs[8];
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return die("GETRESOURCES probe");
	if (res.count_connectors > 8 || res.count_crtcs > 8) return die("too many res");
	res.connector_id_ptr = (uintptr_t) conns; res.crtc_id_ptr = (uintptr_t) crtcs;
	res.encoder_id_ptr = (uintptr_t) encs;    res.fb_id_ptr = (uintptr_t) fbs;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return die("GETRESOURCES");
	if (!res.count_connectors || !res.count_crtcs) return die("no connectors/crtcs");

	/* 2. dumb BO create + map + write + readback — proves GEM alloc + the offset-aware mmap
	 * path (CharDevice::mmapAt -> DRM vma manager). Fixed size, independent of connector modes. */
	struct drm_mode_create_dumb cd; memset(&cd, 0, sizeof cd);
	cd.width = 256; cd.height = 256; cd.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) return die("CREATE_DUMB");
	struct drm_mode_map_dumb md; memset(&md, 0, sizeof md);
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) return die("MAP_DUMB");
	uint32_t *px = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (px == MAP_FAILED) return die("mmap dumb");
	for (uint64_t i = 0; i < cd.size / 4; i++) px[i] = 0xFF2060C0;   /* solid blue-ish */
	if (px[100] != 0xFF2060C0) return die("readback");
	printf("drmtest: dumb-map-write OK\n");

	/* 3. Best-effort KMS modeset: find a connected connector that advertises a mode, then
	 * AddFB + SetCrtc so the screen turns that colour. Skipped (not fatal) if no connector
	 * reports modes — the dumb-map-write + virgl-clear proofs above/below do not need it. */
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
	if (!have_mode) {
		printf("drmtest: kms-skip (no connector modes)\n");
	} else {
		printf("drmtest: mode %ux%u on conn %u\n", mode.hdisplay, mode.vdisplay, conn_id);
		struct drm_mode_create_dumb sd; memset(&sd, 0, sizeof sd);
		sd.width = mode.hdisplay; sd.height = mode.vdisplay; sd.bpp = 32;
		if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &sd)) return die("CREATE_DUMB scanout");
		struct drm_mode_map_dumb sm; memset(&sm, 0, sizeof sm); sm.handle = sd.handle;
		if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &sm)) return die("MAP_DUMB scanout");
		uint32_t *sp = mmap(0, sd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, sm.offset);
		if (sp == MAP_FAILED) return die("mmap scanout");
		for (uint64_t i = 0; i < sd.size / 4; i++) sp[i] = 0xFF2060C0;
		struct drm_mode_fb_cmd fb; memset(&fb, 0, sizeof fb);
		fb.width = sd.width; fb.height = sd.height; fb.bpp = 32; fb.depth = 24;
		fb.pitch = sd.pitch; fb.handle = sd.handle;
		if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb)) return die("ADDFB");
		struct drm_mode_crtc sc; memset(&sc, 0, sizeof sc);
		sc.crtc_id = crtcs[0]; sc.fb_id = fb.fb_id; sc.set_connectors_ptr = (uintptr_t) &conn_id;
		sc.count_connectors = 1; sc.mode = mode; sc.mode_valid = 1;
		if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) return die("SETCRTC");
		printf("drmtest: kms-setcrtc OK\n");
		sleep(3);   /* smoke gate screendumps here */
	}

	return virgl_part();
}
