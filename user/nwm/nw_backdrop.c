#include "nw_backdrop.h"

int nw_rect_empty(nw_rect r) { return r.w <= 0 || r.h <= 0; }

int nw_rect_intersects(nw_rect a, nw_rect b)
{
	if (nw_rect_empty(a) || nw_rect_empty(b)) return 0;
	return a.x < b.x + b.w && b.x < a.x + a.w &&
	       a.y < b.y + b.h && b.y < a.y + a.h;
}

nw_rect nw_rect_expand(nw_rect r, int by)
{
	nw_rect o = { r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by };
	if (o.w < 0) o.w = 0;
	if (o.h < 0) o.h = 0;
	return o;
}

nw_rect nw_rect_clamp(nw_rect r, int sw, int sh)
{
	int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
	int x1 = r.x + r.w, y1 = r.y + r.h;
	if (x1 > sw) x1 = sw;
	if (y1 > sh) y1 = sh;
	nw_rect o = { x0, y0, x1 - x0, y1 - y0 };
	if (o.w < 0) o.w = 0;
	if (o.h < 0) o.h = 0;
	return o;
}

nw_rect nw_rect_intersect(nw_rect a, nw_rect b)
{
	int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
	int ax1 = a.x + a.w, ay1 = a.y + a.h, bx1 = b.x + b.w, by1 = b.y + b.h;
	int x1 = ax1 < bx1 ? ax1 : bx1, y1 = ay1 < by1 ? ay1 : by1;
	nw_rect o = { x0, y0, x1 - x0, y1 - y0 };
	if (o.w < 0) o.w = 0;
	if (o.h < 0) o.h = 0;
	return o;
}

nw_rect nw_rect_union(nw_rect a, nw_rect b)
{
	if (nw_rect_empty(a)) return b;
	if (nw_rect_empty(b)) return a;
	int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
	int ax1 = a.x + a.w, ay1 = a.y + a.h, bx1 = b.x + b.w, by1 = b.y + b.h;
	int x1 = ax1 > bx1 ? ax1 : bx1, y1 = ay1 > by1 ? ay1 : by1;
	nw_rect o = { x0, y0, x1 - x0, y1 - y0 };
	return o;
}

nw_rect nw_cache_rect(nw_rect win, int blur_radius, int sw, int sh)
{
	return nw_rect_clamp(nw_rect_expand(win, blur_radius), sw, sh);
}

void nw_downsample_box(const uint32_t *src, int sw, int sh, int src_stride,
                       uint32_t *dst, int factor)
{
	if (factor < 1) factor = 1;
	int dw = sw / factor, dh = sh / factor;
	for (int dy = 0; dy < dh; dy++) {
		for (int dx = 0; dx < dw; dx++) {
			unsigned r = 0, g = 0, b = 0, n = 0;
			for (int yy = 0; yy < factor; yy++) {
				const uint32_t *row = src + (long) (dy * factor + yy) * src_stride
				                          + dx * factor;
				for (int xx = 0; xx < factor; xx++) {
					uint32_t c = row[xx];
					r += (c >> 16) & 0xff; g += (c >> 8) & 0xff; b += c & 0xff; n++;
				}
			}
			dst[dy * dw + dx] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
		}
	}
}

int nw_backdrop_reusable(nw_rect cached, nw_rect want, int dirty)
{
	if (dirty) return 0;
	return cached.x == want.x && cached.y == want.y &&
	       cached.w == want.w && cached.h == want.h;
}

/* Map output pixel centers back into lo-res space in 16.16 fixed point, then bilinearly
 * blend the four neighbours with nw_lerp (per-channel linear interpolation). For a single
 * lo column/row the step is 0, so every output samples lo[0] (constant) — matches the tests. */
void nw_upsample_bilinear(const uint32_t *lo, int lw, int lh,
                          uint32_t *out, int ow, int oh, int out_stride)
{
	if (lw < 1 || lh < 1 || ow < 1 || oh < 1) return;
	long sx = lw > 1 ? ((long)(lw - 1) << 16) / (ow > 1 ? ow - 1 : 1) : 0;
	long sy = lh > 1 ? ((long)(lh - 1) << 16) / (oh > 1 ? oh - 1 : 1) : 0;
	for (int oy = 0; oy < oh; oy++) {
		long fy = sy * oy;
		int iy0 = (int)(fy >> 16);
		int iy1 = iy0 + 1 < lh ? iy0 + 1 : iy0;
		int ty = (int)(fy & 0xffff) >> 8;            /* 0..255 */
		uint32_t *orow = out + (long) oy * out_stride;
		for (int ox = 0; ox < ow; ox++) {
			long fx = sx * ox;
			int ix0 = (int)(fx >> 16);
			int ix1 = ix0 + 1 < lw ? ix0 + 1 : ix0;
			int tx = (int)(fx & 0xffff) >> 8;        /* 0..255 */
			uint32_t top = nw_lerp(lo[iy0 * lw + ix0], lo[iy0 * lw + ix1], tx, 255);
			uint32_t bot = nw_lerp(lo[iy1 * lw + ix0], lo[iy1 * lw + ix1], tx, 255);
			orow[ox] = nw_lerp(top, bot, ty, 255);
		}
	}
}
