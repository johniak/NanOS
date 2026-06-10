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
