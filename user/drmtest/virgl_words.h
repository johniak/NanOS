/*
 * virgl_words.h — the few virgl command-stream words drmtest needs to emit a CLEAR, built
 * from the vendored protocol header (external/virgl/virgl_protocol.h, pinned to virglrenderer
 * 1.1.1). This is a raw-ABI oracle: the goal is that the host virglrenderer ACCEPTS the stream
 * (EXECBUFFER succeeds, no host error) — pixel-true GL verification comes with Mesa (Task 8).
 */
#pragma once
#include <stdint.h>
#include "virgl_protocol.h"   /* VIRGL_CMD0, VIRGL_CCMD_*, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_*_SIZE */

/* gallium pipe clear-buffer mask (not in virgl_protocol.h) */
#ifndef PIPE_CLEAR_COLOR0
#define PIPE_CLEAR_COLOR0 (1u << 2)
#endif
/* virgl surface/render-target format + bind (from virgl_hw.h; only the two we use) */
#ifndef VIRGL_FORMAT_B8G8R8A8_UNORM
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#endif
#ifndef VIRGL_BIND_RENDER_TARGET
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#endif

/* Emit: CREATE_OBJECT(SURFACE of res_handle) + SET_FRAMEBUFFER_STATE(1 cbuf) + CLEAR(color).
 * Returns the number of dwords written into `c`. */
static inline unsigned build_clear_stream(uint32_t *c, uint32_t res_handle, uint32_t format,
                                          float r, float g, float b, float a)
{
	unsigned n = 0;
	const uint32_t surf = 1;   /* our surface object handle */
	union { float f; uint32_t u; } cv;
	union { double d; uint32_t u[2]; } dv;

	/* CREATE_OBJECT(SURFACE): VIRGL_OBJ_SURFACE_SIZE (5) dwords follow the header */
	c[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE);
	c[n++] = surf;            /* [1] HANDLE      */
	c[n++] = res_handle;      /* [2] RES_HANDLE  */
	c[n++] = format;          /* [3] FORMAT      */
	c[n++] = 0;               /* [4] TEXTURE_LEVEL */
	c[n++] = 0;               /* [5] TEXTURE_LAYERS (first<<0 | last<<16) */

	/* SET_FRAMEBUFFER_STATE: SIZE(nr_cbufs=1) = 3 dwords */
	c[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1));
	c[n++] = 1;               /* NR_CBUFS        */
	c[n++] = 0;               /* ZSURF_HANDLE (none) */
	c[n++] = surf;            /* CBUF_HANDLE(0)  */

	/* CLEAR: VIRGL_OBJ_CLEAR_SIZE (8) dwords = buffers + color[4] + depth(double) + stencil */
	c[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE);
	c[n++] = PIPE_CLEAR_COLOR0;                 /* [1] BUFFERS */
	cv.f = r; c[n++] = cv.u;                    /* [2] COLOR_0 */
	cv.f = g; c[n++] = cv.u;                    /* [3] COLOR_1 */
	cv.f = b; c[n++] = cv.u;                    /* [4] COLOR_2 */
	cv.f = a; c[n++] = cv.u;                    /* [5] COLOR_3 */
	dv.d = 1.0; c[n++] = dv.u[0]; c[n++] = dv.u[1]; /* [6][7] DEPTH (double) */
	c[n++] = 0;                                 /* [8] STENCIL */
	return n;
}
