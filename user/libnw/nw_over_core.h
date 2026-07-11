/*
 * nw_over_core.h — the straight-alpha src-over blend core, shared verbatim between the OS build
 * (nw_gfx.h includes this so every TU that includes nw_gfx.h gets nw_over_pixel, exactly like
 * nw_blend8 in nw_gfx.h) and the host-only unit test (tests/host-argb.c, which supplies its own
 * minimal `struct nw_surface` and defines NW_ARGB_TEST_HOST so this header skips nw_gfx.h).
 * static inline: each TU gets its own private copy, no cross-TU symbol, no link step needed by
 * the host test.
 */
#ifndef NW_OVER_CORE_H
#define NW_OVER_CORE_H

#include <stdint.h>
#include <stddef.h>   /* size_t */
#ifndef NW_ARGB_TEST_HOST
#include "nw_gfx.h"   /* struct nw_surface (already included by callers, harmless re-include) */
#endif

/* Straight-alpha src-over that also accumulates destination alpha:
 *   outA = sa + da*(1-sa);  outC = (sc*sa + dc*da*(1-sa)) / outA
 * Dest is straight ARGB (the GL shader mixes ink as mix(glass, rgb, a)).
 *
 * Bounds check reuses the SAME "scissor active only when clip_x1>clip_x0 && clip_y1>clip_y0,
 * else full surface" convention as nw_put_pixel/nw_blend_pixel (see nw_gfx.h's nw_bounds) rather
 * than a raw clip_x0..clip_x1 compare: a zero-initialised (noclip) surface — e.g. the window
 * buffer handed out by nw_win_surface(), which calls nw_surface_noclip() — must stay fully
 * drawable, not collapse to a single pixel at the origin. */
static inline void nw_over_pixel(const struct nw_surface *s, int x, int y, uint32_t argb)
{
	int bx0 = 0, by0 = 0, bx1 = s->w, by1 = s->h;
	if (s->clip_x1 > s->clip_x0 && s->clip_y1 > s->clip_y0) {   /* scissor active */
		if (s->clip_x0 > bx0) bx0 = s->clip_x0;
		if (s->clip_y0 > by0) by0 = s->clip_y0;
		if (s->clip_x1 < bx1) bx1 = s->clip_x1;
		if (s->clip_y1 < by1) by1 = s->clip_y1;
	}
	if (x < bx0 || y < by0 || x >= bx1 || y >= by1) return;
	uint32_t *d = s->px + (size_t) y * s->stride + x;
	unsigned sa = argb >> 24;
	if (!sa) return;
	if (sa == 255) { *d = argb; return; }
	uint32_t dst = *d;
	unsigned da  = dst >> 24;
	unsigned ra  = sa + ((da * (255 - sa) + 127) / 255);          /* out alpha */
	if (!ra) { *d = 0; return; }
	unsigned wd = da * (255 - sa) / 255;                          /* dst weight */
	unsigned sr = (argb >> 16) & 0xff, sg = (argb >> 8) & 0xff, sb = argb & 0xff;
	unsigned dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
	unsigned r = (sr * sa + dr * wd) / ra;
	unsigned g = (sg * sa + dg * wd) / ra;
	unsigned b = (sb * sa + db * wd) / ra;
	*d = ((uint32_t) ra << 24) | (r << 16) | (g << 8) | b;
}

#endif /* NW_OVER_CORE_H */
