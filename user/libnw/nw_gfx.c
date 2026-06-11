/*
 * nw_gfx.c — implementation of the 32bpp software rasterizer (see nw_gfx.h). Mirrors the
 * semantics of drivers/Framebuffer.cpp (BGRX pixels, MSB-leftmost 8x16 glyphs) but works on
 * a pixel-stride surface in userland. Glyph data is the shared userland font nx_font8x16
 * (user/term/vtfont.c), the public-domain IBM VGA 8x16 set.
 */
#include "nw_gfx.h"

/* The shared userland console font: 256 glyphs, 16 bytes each, MSB = leftmost pixel. */
extern const unsigned char nx_font8x16[256][16];

void nw_surface_clip(struct nw_surface *s, int x, int y, int w, int h)
{
	s->clip_x0 = x; s->clip_y0 = y; s->clip_x1 = x + w; s->clip_y1 = y + h;
}
void nw_surface_noclip(struct nw_surface *s)
{
	s->clip_x0 = s->clip_y0 = s->clip_x1 = s->clip_y1 = 0;
}

/* The drawable bounds: the surface [0,w)x[0,h) intersected with the scissor when it is active. */
static void nw_bounds(const struct nw_surface *s, int *x0, int *y0, int *x1, int *y1)
{
	*x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h;
	if (s->clip_x1 > s->clip_x0 && s->clip_y1 > s->clip_y0) {   /* scissor active */
		if (s->clip_x0 > *x0) *x0 = s->clip_x0;
		if (s->clip_y0 > *y0) *y0 = s->clip_y0;
		if (s->clip_x1 < *x1) *x1 = s->clip_x1;
		if (s->clip_y1 < *y1) *y1 = s->clip_y1;
	}
}

void nw_put_pixel(const struct nw_surface *s, int x, int y, uint32_t rgb)
{
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	if (x < bx0 || y < by0 || x >= bx1 || y >= by1)
		return;
	s->px[(long) y * s->stride + x] = rgb;
}

void nw_fill_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t rgb)
{
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	if (x0 < bx0) x0 = bx0;
	if (y0 < by0) y0 = by0;
	if (x1 > bx1) x1 = bx1;
	if (y1 > by1) y1 = by1;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = s->px + (long) yy * s->stride;
		for (int xx = x0; xx < x1; xx++)
			row[xx] = rgb;
	}
}

void nw_draw_char(const struct nw_surface *s, int x, int y, unsigned char ch,
                  uint32_t fg, uint32_t bg)
{
	const unsigned char *glyph = nx_font8x16[ch];
	for (int row = 0; row < NW_FONT_H; row++) {
		unsigned char bits = glyph[row];
		for (int col = 0; col < NW_FONT_W; col++)
			nw_put_pixel(s, x + col, y + row, (bits & (0x80u >> col)) ? fg : bg);
	}
}

int nw_draw_text(const struct nw_surface *s, int x, int y, const char *str,
                 uint32_t fg, uint32_t bg)
{
	for (; *str; str++) {
		nw_draw_char(s, x, y, (unsigned char) *str, fg, bg);
		x += NW_FONT_W;
	}
	return x;
}

/* Transparent-background text: only the glyph's set pixels are drawn (in fg), leaving the
 * backdrop intact — for titles/labels over gradients or translucent material. Returns end x. */
int nw_text(const struct nw_surface *s, int x, int y, const char *str, uint32_t fg)
{
	for (; *str; str++) {
		const unsigned char *glyph = nx_font8x16[(unsigned char) *str];
		for (int row = 0; row < NW_FONT_H; row++) {
			unsigned char bits = glyph[row];
			for (int col = 0; col < NW_FONT_W; col++)
				if (bits & (0x80u >> col)) nw_put_pixel(s, x + col, y + row, fg);
		}
		x += NW_FONT_W;
	}
	return x;
}

void nw_blit(const struct nw_surface *dst, int dx, int dy,
             const struct nw_surface *src, int sx, int sy, int w, int h)
{
	/* clip the source rect to src bounds, shifting the destination in step */
	if (sx < 0) { w += sx; dx -= sx; sx = 0; }
	if (sy < 0) { h += sy; dy -= sy; sy = 0; }
	if (sx + w > src->w) w = src->w - sx;
	if (sy + h > src->h) h = src->h - sy;
	/* clip the destination rect to dst drawable bounds (surface ∩ scissor), shifting src */
	int bx0, by0, bx1, by1;
	nw_bounds(dst, &bx0, &by0, &bx1, &by1);
	if (dx < bx0) { int d = bx0 - dx; w -= d; sx += d; dx = bx0; }
	if (dy < by0) { int d = by0 - dy; h -= d; sy += d; dy = by0; }
	if (dx + w > bx1) w = bx1 - dx;
	if (dy + h > by1) h = by1 - dy;
	if (w <= 0 || h <= 0)
		return;
	for (int r = 0; r < h; r++) {
		const uint32_t *srow = src->px + (long) (sy + r) * src->stride + sx;
		uint32_t       *drow = dst->px + (long) (dy + r) * dst->stride + dx;
		for (int c = 0; c < w; c++)
			drow[c] = srow[c];
	}
}

/* ---- alpha / gradients / rounded rects / blur ------------------------------------- */

uint32_t nw_mix(uint32_t dst, uint32_t src, int a)   /* src over dst at coverage a (0..255) */
{
	if (a <= 0)   return dst;
	if (a >= 255) return src;
	int ia = 255 - a;
	int r = (((src >> 16) & 0xff) * a + ((dst >> 16) & 0xff) * ia) / 255;
	int g = (((src >>  8) & 0xff) * a + ((dst >>  8) & 0xff) * ia) / 255;
	int b = (( src        & 0xff) * a + ( dst        & 0xff) * ia) / 255;
	return (uint32_t) ((r << 16) | (g << 8) | b);
}
uint32_t nw_lerp(uint32_t a, uint32_t b, int t, int n)   /* a..b as t/n (0..n) */
{
	if (n <= 0) return a;
	int r = (((a >> 16) & 0xff) * (n - t) + ((b >> 16) & 0xff) * t) / n;
	int g = (((a >>  8) & 0xff) * (n - t) + ((b >>  8) & 0xff) * t) / n;
	int bl= (( a        & 0xff) * (n - t) + ( b        & 0xff) * t) / n;
	return (uint32_t) ((r << 16) | (g << 8) | bl);
}
#define mix nw_mix
#define lerp nw_lerp

void nw_blend_pixel(const struct nw_surface *s, int x, int y, uint32_t rgb, int a)
{
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	if (x < bx0 || y < by0 || x >= bx1 || y >= by1)
		return;
	uint32_t *p = &s->px[(long) y * s->stride + x];
	*p = mix(*p, rgb, a);
}

void nw_blend_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t rgb, int a)
{
	if (a >= 255) { nw_fill_rect(s, x, y, w, h, rgb); return; }
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	int x0 = x < bx0 ? bx0 : x, y0 = y < by0 ? by0 : y;
	int x1 = x + w > bx1 ? bx1 : x + w, y1 = y + h > by1 ? by1 : y + h;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = s->px + (long) yy * s->stride;
		for (int xx = x0; xx < x1; xx++)
			row[xx] = mix(row[xx], rgb, a);
	}
}

void nw_vgrad_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bot)
{
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	int x0 = x < bx0 ? bx0 : x, y0 = y < by0 ? by0 : y;
	int x1 = x + w > bx1 ? bx1 : x + w, y1 = y + h > by1 ? by1 : y + h;
	int span = h > 1 ? h - 1 : 1;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t c = lerp(top, bot, yy - y, span);
		uint32_t *row = s->px + (long) yy * s->stride;
		for (int xx = x0; xx < x1; xx++)
			row[xx] = c;
	}
}

/* Coverage (0..255) of a pixel whose centre is at distance d (px) from a circle of radius r:
 * 1 inside, 0 outside, a 1px linear ramp across the edge for anti-aliasing. */
static int edge_cov(float d, int r)
{
	float e = (float) r - d;       /* >0 inside */
	if (e >= 0.5f)  return 255;
	if (e <= -0.5f) return 0;
	return (int) ((e + 0.5f) * 255.0f);
}

void nw_fill_round(const struct nw_surface *s, int x, int y, int w, int h, int r, uint32_t rgb, int a)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (r < 1) { nw_blend_rect(s, x, y, w, h, rgb, a); return; }
	/* straight middle band + top/bottom strips between the corners */
	nw_blend_rect(s, x, y + r, w, h - 2 * r, rgb, a);
	nw_blend_rect(s, x + r, y, w - 2 * r, r, rgb, a);
	nw_blend_rect(s, x + r, y + h - r, w - 2 * r, r, rgb, a);
	/* four AA corners: centre of each quarter-circle is r px in from the corner */
	int cx[4] = { x + r, x + w - r, x + r, x + w - r };
	int cy[4] = { y + r, y + r, y + h - r, y + h - r };
	for (int k = 0; k < 4; k++) {
		int ox = (k & 1) ? cx[k] : cx[k] - r;     /* corner box top-left */
		int oy = (k & 2) ? cy[k] : cy[k] - r;
		for (int yy = 0; yy < r; yy++)
			for (int xx = 0; xx < r; xx++) {
				float dx = (ox + xx) + 0.5f - cx[k];
				float dy = (oy + yy) + 0.5f - cy[k];
				int cov = edge_cov(__builtin_sqrtf(dx * dx + dy * dy), r);
				if (cov) nw_blend_pixel(s, ox + xx, oy + yy, rgb, cov * a / 255);
			}
	}
}

void nw_stroke_round(const struct nw_surface *s, int x, int y, int w, int h, int r, uint32_t rgb, int a)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	/* straight edges */
	nw_blend_rect(s, x + r, y, w - 2 * r, 1, rgb, a);
	nw_blend_rect(s, x + r, y + h - 1, w - 2 * r, 1, rgb, a);
	nw_blend_rect(s, x, y + r, 1, h - 2 * r, rgb, a);
	nw_blend_rect(s, x + w - 1, y + r, 1, h - 2 * r, rgb, a);
	if (r < 1) return;
	int cx[4] = { x + r, x + w - r, x + r, x + w - r };
	int cy[4] = { y + r, y + r, y + h - r, y + h - r };
	for (int k = 0; k < 4; k++) {
		int ox = (k & 1) ? cx[k] : cx[k] - r, oy = (k & 2) ? cy[k] : cy[k] - r;
		for (int yy = 0; yy < r; yy++)
			for (int xx = 0; xx < r; xx++) {
				float dx = (ox + xx) + 0.5f - cx[k], dy = (oy + yy) + 0.5f - cy[k];
				float d = __builtin_sqrtf(dx * dx + dy * dy);
				int outer = edge_cov(d, r);           /* inside the outer arc */
				int inner = edge_cov(d, r - 1);        /* inside the 1px-smaller arc */
				int cov = outer - inner;               /* the 1px ring */
				if (cov > 0) nw_blend_pixel(s, ox + xx, oy + yy, rgb, cov * a / 255);
			}
	}
}

/* In-place separable box blur. Bounded by the scissor/surface; `radius` px, `passes` times. */
void nw_blur_rect(const struct nw_surface *s, int x, int y, int w, int h, int radius, int passes)
{
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	int x0 = x < bx0 ? bx0 : x, y0 = y < by0 ? by0 : y;
	int x1 = x + w > bx1 ? bx1 : x + w, y1 = y + h > by1 ? by1 : y + h;
	int rw = x1 - x0, rh = y1 - y0;
	if (rw <= 0 || rh <= 0 || radius < 1) return;
	int line = rw > rh ? rw : rh;
	uint32_t tmp[2048];
	if (line > 2048) return;                    /* region too wide to blur (no heap here) */
	for (int p = 0; p < passes; p++) {
		for (int yy = y0; yy < y1; yy++) {       /* horizontal pass */
			uint32_t *row = s->px + (long) yy * s->stride;
			for (int xx = x0; xx < x1; xx++) {
				int rsum = 0, gsum = 0, bsum = 0, n = 0;
				int a = xx - radius < x0 ? x0 : xx - radius;
				int b = xx + radius >= x1 ? x1 - 1 : xx + radius;
				for (int i = a; i <= b; i++) {
					uint32_t c = row[i];
					rsum += (c >> 16) & 0xff; gsum += (c >> 8) & 0xff; bsum += c & 0xff; n++;
				}
				tmp[xx - x0] = (uint32_t) (((rsum / n) << 16) | ((gsum / n) << 8) | (bsum / n));
			}
			for (int xx = x0; xx < x1; xx++) row[xx] = tmp[xx - x0];
		}
		for (int xx = x0; xx < x1; xx++) {       /* vertical pass */
			for (int yy = y0; yy < y1; yy++) {
				int rsum = 0, gsum = 0, bsum = 0, n = 0;
				int a = yy - radius < y0 ? y0 : yy - radius;
				int b = yy + radius >= y1 ? y1 - 1 : yy + radius;
				for (int i = a; i <= b; i++) {
					uint32_t c = s->px[(long) i * s->stride + xx];
					rsum += (c >> 16) & 0xff; gsum += (c >> 8) & 0xff; bsum += c & 0xff; n++;
				}
				tmp[yy - y0] = (uint32_t) (((rsum / n) << 16) | ((gsum / n) << 8) | (bsum / n));
			}
			for (int yy = y0; yy < y1; yy++) s->px[(long) yy * s->stride + xx] = tmp[yy - y0];
		}
	}
}
