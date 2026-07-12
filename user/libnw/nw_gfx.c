/*
 * nw_gfx.c — implementation of the 32bpp software rasterizer (see nw_gfx.h). Mirrors the
 * semantics of drivers/Framebuffer.cpp (BGRX pixels, MSB-leftmost 8x16 glyphs) but works on
 * a pixel-stride surface in userland. Glyph data is the shared userland font nx_font8x16
 * (user/term/vtfont.c), the public-domain IBM VGA 8x16 set.
 */
#include "nw_gfx.h"
#include <string.h>   /* memcpy for the row blit */

/* The shared userland console font: 256 glyphs, 16 bytes each, MSB = leftmost pixel. Used as the
 * fixed terminal font AND as the fallback when no TTF UI font is loaded. */
extern const unsigned char nx_font8x16[256][16];

#include "nwfont.h"
#include <fcntl.h>
#include <unistd.h>
#define NW_UI_FONT_PX     15
#define NW_SETTINGS_PATH  "/disks/main/nanos/config/settings.yaml"
#define NW_FONTS_DIR      "/disks/main/nanos/share/fonts"
#define NW_UI_FONT_DEFAULT "UISans-Regular.ttf"

/* Load the UI font named in settings.yaml (the `ui_font:` line) from NW_FONTS_DIR; default UISans.
 * A tiny self-contained scanner — libnw must NOT depend on the nw_settings module (it isn't linked
 * into libnw.elf). Every process does this so the chosen font applies everywhere. */
static void load_ui_from_settings(void)
{
    char name[64];
    int nl = 0;
    int fd = open(NW_SETTINGS_PATH, O_RDONLY);
    if (fd >= 0) {
        char b[1024];
        int n = (int) read(fd, b, sizeof b - 1);
        close(fd);
        if (n > 0) {
            b[n] = 0;
            for (int i = 0; i < n; ) {
                int s = i;
                while (i < n && b[i] != '\n') i++;
                const char *L = b + s;
                int len = i - s;
                int p = 0;
                while (p < len && (L[p] == ' ' || L[p] == '\t')) p++;
                const char *key = "ui_font:";
                int kl = 8, match = (len - p >= kl);
                for (int j = 0; match && j < kl; j++) if (L[p + j] != key[j]) match = 0;
                if (match) {
                    int v = p + kl;
                    while (v < len && (L[v] == ' ' || L[v] == '\t')) v++;
                    int e = len;
                    while (e > v && (L[e - 1] == ' ' || L[e - 1] == '\t' || L[e - 1] == '\r')) e--;
                    nl = 0;
                    for (int j = v; j < e && nl < 63; j++) name[nl++] = L[j];
                    name[nl] = 0;
                }
                i++;   /* skip the newline */
            }
        }
    }
    const char *nm = nl ? name : NW_UI_FONT_DEFAULT;
    char path[160];
    int i = 0;
    for (const char *d = NW_FONTS_DIR; *d && i < 120; d++) path[i++] = *d;
    path[i++] = '/';
    for (int j = 0; nm[j] && i < 159; j++) path[i++] = nm[j];
    path[i] = 0;
    nwfont_set(NWFONT_UI, path, NW_UI_FONT_PX);
}

/* Lazily load the proportional UI font on first text use, so every process gets it with no
 * per-app init call. */
static int g_ui_font_tried;
static void ui_font_autoinit(void)
{
    if (g_ui_font_tried) return;
    g_ui_font_tried = 1;
    load_ui_from_settings();
}

/* Reload the UI font (Settings font switch). px<=0 keeps the default size. */
void nw_font_set_ui(const char *path, int px)
{
    g_ui_font_tried = 1;
    nwfont_set(NWFONT_UI, path, px > 0 ? px : NW_UI_FONT_PX);
}

/* Re-read settings.yaml and (re)load the UI font — the compositor calls this on a settings reload
 * so a font change applies live to chrome. */
void nw_font_reload_from_settings(void)
{
    g_ui_font_tried = 1;
    load_ui_from_settings();
}

/* The fixed-cell monospace font (text inputs); sized to fit the 8x16 cell (advance ~= NW_FONT_W). */
#define NW_MONO_FONT_PATH "/disks/main/nanos/share/fonts/Mono-Regular.ttf"
#define NW_MONO_FONT_PX   13
static int g_mono_font_tried;
static void mono_font_autoinit(void)
{
    if (g_mono_font_tried) return;
    g_mono_font_tried = 1;
    nwfont_set(NWFONT_MONO, NW_MONO_FONT_PATH, NW_MONO_FONT_PX);
}

/* Coverage -> alpha remap for glyph AA: blending sRGB with raw coverage renders dark
 * text thin and light text fat; remapping by a^(1/1.43) is the standard perceptual
 * compromise (Photoshop/Skia). Glyph coverage only — icon alpha is real alpha.       */
static uint8_t s_cov143[256];
static int     s_cov143_init;
const uint8_t *nw_cov143(void)
{
	if (!s_cov143_init) {
		for (int i = 0; i < 256; i++) {
			/* pow(i/255, 1/1.43) = a^0.699 without libm pow (freestanding):
			 * sqrt chain a^0.5 * a^0.125 * a^0.0625 = a^0.6875 — exponent error
			 * < 0.012, well under a coverage step. __builtin_sqrt is a compiler
			 * intrinsic (already used as __builtin_sqrtf below), not a linked libm
			 * call, so it works in this freestanding userland. */
			double a = i / 255.0;
			double s1 = __builtin_sqrt(a);    /* a^0.5    */
			double s2 = __builtin_sqrt(s1);   /* a^0.25   */
			double s3 = __builtin_sqrt(s2);   /* a^0.125  */
			double s4 = __builtin_sqrt(s3);   /* a^0.0625 */
			double r  = s1 * s3 * s4;         /* a^0.6875 ~= a^(1/1.43) */
			s_cov143[i] = (uint8_t)(r * 255.0 + 0.5);
		}
		s_cov143[0] = 0; s_cov143[255] = 255;
		s_cov143_init = 1;
	}
	return s_cov143;
}

/* Draw one char with a TRANSPARENT background in the monospace font (AA), in a fixed NW_FONT_W
 * cell at (x,y); the caller advances by NW_FONT_W. Text inputs use this so their grid math
 * (caret/selection by NW_FONT_W) stays exact while glyphs render smoothly. VGA 1-bit fallback. */
void nw_draw_char_t(const struct nw_surface *s, int x, int y, unsigned char ch, uint32_t fg)
{
    mono_font_autoinit();
    if (nwfont_loaded(NWFONT_MONO)) {
        const struct nwfont_glyph *g = nwfont_get(NWFONT_MONO, ch);
        if (g && g->cov) {
            const uint8_t *lut = nw_cov143();
            int baseline = y + nwfont_ascent(NWFONT_MONO);
            for (int gy = 0; gy < g->h; gy++) {
                const unsigned char *covrow = g->cov + (long) gy * g->w;
                int py = baseline + g->top + gy;
                for (int gx = 0; gx < g->w; gx++)
                    if (covrow[gx]) nw_blend_pixel(s, x + g->bx + gx, py, fg, lut[covrow[gx]]);
            }
        }
        return;
    }
    const unsigned char *glyph = nx_font8x16[ch];   /* VGA 1-bit fallback */
    for (int row = 0; row < NW_FONT_H; row++)
        for (int col = 0; col < NW_FONT_W; col++)
            if (glyph[row] & (0x80u >> col)) nw_put_pixel(s, x + col, y + row, fg);
}

/* Pixel width of a string in the current UI font (proportional), or the 1-bit fallback width. */
int nw_text_w(const char *str)
{
    ui_font_autoinit();
    if (nwfont_loaded(NWFONT_UI)) return nwfont_text_w(NWFONT_UI, str);
    int w = 0;
    for (; str && *str; str++) w += NW_FONT_W;
    return w;
}

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

void nw_surface_bounds(const struct nw_surface *s, int *x0, int *y0, int *x1, int *y1)
{
	nw_bounds(s, x0, y0, x1, y1);
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
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	if (x >= bx0 && y >= by0 && x + NW_FONT_W <= bx1 && y + NW_FONT_H <= by1) {
		/* fast path: whole glyph is inside the clip — write rows directly, no per-pixel clip */
		for (int row = 0; row < NW_FONT_H; row++) {
			unsigned char bits = glyph[row];
			uint32_t *p = s->px + (long) (y + row) * s->stride + x;
			for (int col = 0; col < NW_FONT_W; col++)
				p[col] = (bits & (0x80u >> col)) ? fg : bg;
		}
		return;
	}
	for (int row = 0; row < NW_FONT_H; row++) {   /* edge case: clip per pixel */
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
	ui_font_autoinit();
	if (nwfont_loaded(NWFONT_UI)) {
		/* proportional anti-aliased UI font: blend each glyph's coverage at its advance.
		 * y is the line-box top; the baseline sits at y + ascent. */
		const uint8_t *lut = nw_cov143();
		int baseline = y + nwfont_ascent(NWFONT_UI);
		for (; *str; str++) {
			const struct nwfont_glyph *g = nwfont_get(NWFONT_UI, (unsigned char) *str);
			if (!g) continue;
			if (g->cov) {
				for (int gy = 0; gy < g->h; gy++) {
					const unsigned char *covrow = g->cov + (long) gy * g->w;
					int py = baseline + g->top + gy;
					for (int gx = 0; gx < g->w; gx++) {
						unsigned char a = covrow[gx];
						if (a) nw_blend_pixel(s, x + g->bx + gx, py, fg, lut[a]);
					}
				}
			}
			x += g->advance;
		}
		return x;
	}
	/* fallback: 1-bit VGA font (no TTF loaded) */
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	for (; *str; str++) {
		const unsigned char *glyph = nx_font8x16[(unsigned char) *str];
		if (x >= bx0 && y >= by0 && x + NW_FONT_W <= bx1 && y + NW_FONT_H <= by1) {
			for (int row = 0; row < NW_FONT_H; row++) {
				unsigned char bits = glyph[row];
				uint32_t *p = s->px + (long) (y + row) * s->stride + x;
				for (int col = 0; col < NW_FONT_W; col++)
					if (bits & (0x80u >> col)) p[col] = fg;
			}
		} else {
			for (int row = 0; row < NW_FONT_H; row++)
				for (int col = 0; col < NW_FONT_W; col++)
					if (glyph[row] & (0x80u >> col)) nw_put_pixel(s, x + col, y + row, fg);
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
		memcpy(drow, srow, (size_t) w * sizeof(uint32_t));   /* row at a time, not per pixel */
	}
}

/* ---- alpha / gradients / rounded rects / blur ------------------------------------- */

uint32_t nw_mix(uint32_t dst, uint32_t src, int a)   /* src over dst at coverage a (0..255) */
{
	if (a <= 0)   return dst;
	if (a >= 255) return src;
	return nw_blend8(dst, src, (unsigned) a);        /* shared RB-paired blend (see nw_gfx.h) */
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

/* nw_over_ring: the 1px AA rounded ring of nw_stroke_round, but as a straight-alpha
 * src-over write (glass ink) — a rim that can be BRIGHTER than the fill under it. */
void nw_over_ring(const struct nw_surface *s, int x, int y, int w, int h, int r, uint32_t argb)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	/* straight edges */
	nw_over_rect(s, x + r, y, w - 2 * r, 1, argb);
	nw_over_rect(s, x + r, y + h - 1, w - 2 * r, 1, argb);
	nw_over_rect(s, x, y + r, 1, h - 2 * r, argb);
	nw_over_rect(s, x + w - 1, y + r, 1, h - 2 * r, argb);
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
				if (cov > 0) nw_over_pixel(s, ox + xx, oy + yy,
					((uint32_t)(cov * (argb >> 24) / 255) << 24) | (argb & 0x00ffffffu));
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

/* ---- straight-alpha ARGB ink primitives (see nw_gfx.h) ---------------------------------------
 * nw_over_pixel itself is the static inline in nw_over_core.h (pulled in via nw_gfx.h); nothing
 * to wrap here — every TU that includes nw_gfx.h gets its own copy, same as nw_blend8. */

void nw_clear_argb(const struct nw_surface *s, int x, int y, int w, int h, uint32_t argb)
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
			row[xx] = argb;
	}
}

void nw_over_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t argb)
{
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	if (x0 < bx0) x0 = bx0;
	if (y0 < by0) y0 = by0;
	if (x1 > bx1) x1 = bx1;
	if (y1 > by1) y1 = by1;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++)
			nw_over_pixel(s, xx, yy, argb);
}

void nw_over_round(const struct nw_surface *s, int x, int y, int w, int h, int r, uint32_t argb)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (r < 1) { nw_over_rect(s, x, y, w, h, argb); return; }
	unsigned sa = argb >> 24;
	uint32_t rgb = argb & 0x00ffffffu;
	/* straight middle band + top/bottom strips between the corners: full ink alpha, no AA */
	nw_over_rect(s, x, y + r, w, h - 2 * r, argb);
	nw_over_rect(s, x + r, y, w - 2 * r, r, argb);
	nw_over_rect(s, x + r, y + h - r, w - 2 * r, r, argb);
	/* four AA corners: same geometry as nw_fill_round, coverage scales the ink alpha */
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
				if (cov) {
					unsigned a = (unsigned) (cov * (int) sa) / 255;
					nw_over_pixel(s, ox + xx, oy + yy, (a << 24) | rgb);
				}
			}
	}
}

void nw_over_round_soft(const struct nw_surface *s, int x, int y, int w, int h, int r,
                        uint32_t argb, int feather)
{
	if (w <= 0 || h <= 0) return;
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (feather < 1) { nw_over_round(s, x, y, w, h, r, argb); return; }
	unsigned sa = argb >> 24;
	uint32_t rgb = argb & 0x00ffffffu;
	/* rounded-box SDF against the whole rect: sd < 0 inside; coverage ramps 0..1 across the
	 * `feather` px just inside the edge (smoothstepped so the fade has no visible start/stop
	 * line). Pixels deeper than the ramp take the full-ink fast path (no sqrt, no per-pixel
	 * alpha math beyond nw_over_pixel) — the ramp only ever touches a thin border band. */
	int bx0, by0, bx1, by1;
	nw_bounds(s, &bx0, &by0, &bx1, &by1);
	int x0 = x < bx0 ? bx0 : x, y0 = y < by0 ? by0 : y;
	int x1 = x + w > bx1 ? bx1 : x + w, y1 = y + h > by1 ? by1 : y + h;
	float hw = w * 0.5f, hh = h * 0.5f;
	float cx = x + hw, cy = y + hh, fr = (float) r, ff = (float) feather;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			float qx = xx + 0.5f - cx, qy = yy + 0.5f - cy;
			if (qx < 0) qx = -qx;
			if (qy < 0) qy = -qy;
			qx -= hw - fr;  qy -= hh - fr;
			float sd;
			if (qx > 0.0f && qy > 0.0f)
				sd = __builtin_sqrtf(qx * qx + qy * qy) - fr;   /* corner arc */
			else
				sd = (qx > qy ? qx : qy) - fr;                  /* straight edge */
			float t = -sd / ff;
			if (t <= 0.0f) continue;
			if (t >= 1.0f) { nw_over_pixel(s, xx, yy, argb); continue; }
			t = t * t * (3.0f - 2.0f * t);
			unsigned a = (unsigned) ((float) sa * t + 0.5f);
			if (a) nw_over_pixel(s, xx, yy, (a << 24) | rgb);
		}
}

void nw_text_argb(const struct nw_surface *s, int x, int y, const char *str, uint32_t argb)
{
	ui_font_autoinit();
	unsigned sa = argb >> 24;
	uint32_t rgb = argb & 0x00ffffffu;
	if (nwfont_loaded(NWFONT_UI)) {
		/* proportional anti-aliased UI font: same baseline/advance math as nw_text, but write
		 * straight alpha (cov143-remapped coverage scaled by the ink's own alpha) instead of
		 * blending into an alpha-less dest. */
		const uint8_t *lut = nw_cov143();
		int baseline = y + nwfont_ascent(NWFONT_UI);
		for (; *str; str++) {
			const struct nwfont_glyph *g = nwfont_get(NWFONT_UI, (unsigned char) *str);
			if (!g) continue;
			if (g->cov) {
				for (int gy = 0; gy < g->h; gy++) {
					const unsigned char *covrow = g->cov + (long) gy * g->w;
					int py = baseline + g->top + gy;
					for (int gx = 0; gx < g->w; gx++) {
						unsigned char c = covrow[gx];
						if (c) {
							unsigned a = lut[c];
							a = (a * sa) / 255;
							nw_over_pixel(s, x + g->bx + gx, py, (a << 24) | rgb);
						}
					}
				}
			}
			x += g->advance;
		}
		return;
	}
	/* fallback: 1-bit VGA font (no TTF loaded) — full glyph coverage, scaled by argb's alpha */
	for (; *str; str++) {
		const unsigned char *glyph = nx_font8x16[(unsigned char) *str];
		for (int row = 0; row < NW_FONT_H; row++)
			for (int col = 0; col < NW_FONT_W; col++)
				if (glyph[row] & (0x80u >> col))
					nw_over_pixel(s, x + col, y + row, argb);
		x += NW_FONT_W;
	}
}

int nw_text_argb_w(const char *str)
{
	return nw_text_w(str);
}
