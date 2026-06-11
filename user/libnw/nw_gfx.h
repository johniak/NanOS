/*
 * nw_gfx.h — a tiny 32bpp software rasterizer over an in-memory pixel surface, shared by
 * the compositor (drawing decorations + compositing windows) and clients (drawing into
 * their own window buffer via libnw). Pixels are 0x00RRGGBB stored as uint32 (BGRX in
 * memory on little-endian, matching /dev/fb0 — see drivers/Framebuffer.cpp). Pure logic,
 * fully clipped, host-tested.
 */
#ifndef NW_GFX_H
#define NW_GFX_H

#include <stdint.h>

#define NW_FONT_W 8
#define NW_FONT_H 16

/* A drawable surface: a pixel buffer with a row stride (in pixels, >= w). */
struct nw_surface {
	uint32_t *px;   /* pixels, 0x00RRGGBB each */
	int       w, h; /* size in pixels */
	int       stride; /* row stride in PIXELS */
	/* Optional scissor: drawing is confined to this rect (in surface pixels). ACTIVE only
	 * when clip_x1 > clip_x0 && clip_y1 > clip_y0; a zero-initialised surface has NO scissor
	 * (draws to the full w*h). Use nw_surface_clip / nw_surface_noclip to set it. This is how
	 * the compositor recomposes only the damaged region instead of the whole scene. */
	int       clip_x0, clip_y0, clip_x1, clip_y1;
};

/* Confine subsequent drawing on `s` to the rectangle (x,y,w,h) (intersected with the surface
 * by the primitives). nw_surface_noclip removes the scissor (full-surface drawing). */
void nw_surface_clip(struct nw_surface *s, int x, int y, int w, int h);
void nw_surface_noclip(struct nw_surface *s);
/* The current drawable bounds (surface ∩ scissor) as [x0,x1) × [y0,y1) — for fast loops. */
void nw_surface_bounds(const struct nw_surface *s, int *x0, int *y0, int *x1, int *y1);

/* All coordinates may be partially or fully off-surface; everything clips. */
void nw_put_pixel(const struct nw_surface *s, int x, int y, uint32_t rgb);
void nw_fill_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t rgb);
void nw_draw_char(const struct nw_surface *s, int x, int y, unsigned char ch,
                  uint32_t fg, uint32_t bg);
/* Draws str left-to-right at 8px advance; no wrapping. Returns the x past the last glyph. */
int  nw_draw_text(const struct nw_surface *s, int x, int y, const char *str,
                  uint32_t fg, uint32_t bg);
/* Transparent-background text: only the glyph's lit pixels are painted (fg), backdrop kept. */
int  nw_text(const struct nw_surface *s, int x, int y, const char *str, uint32_t fg);
/* Copy a w*h block from src(sx,sy) to dst(dx,dy). Clips against BOTH surfaces (negative
 * offsets included) — the compositor's window-into-backbuffer blit. */
void nw_blit(const struct nw_surface *dst, int dx, int dy,
             const struct nw_surface *src, int sx, int sy, int w, int h);

/* ---- modern compositing: alpha, gradients, rounded rects, blur (all clipped + scissored) ---- */

/* Colour math (0x00RRGGBB): src over dst at coverage a (0..255); linear a..b as t/n. */
uint32_t nw_mix(uint32_t dst, uint32_t src, int a);
uint32_t nw_lerp(uint32_t a, uint32_t b, int t, int n);

/* Alpha-blend rgb over the surface at coverage a (0..255). Per-pixel and filled-rect forms. */
void nw_blend_pixel(const struct nw_surface *s, int x, int y, uint32_t rgb, int a);
void nw_blend_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t rgb, int a);

/* Vertical linear gradient fill (opaque) from `top` colour to `bot` colour down the rect. */
void nw_vgrad_rect(const struct nw_surface *s, int x, int y, int w, int h,
                   uint32_t top, uint32_t bot);

/* Rounded-rectangle fill, radius r, blended at alpha a (255 = opaque); corners anti-aliased.
 * nw_stroke_round draws a 1px AA border instead of filling. */
void nw_fill_round(const struct nw_surface *s, int x, int y, int w, int h, int r,
                   uint32_t rgb, int a);
void nw_stroke_round(const struct nw_surface *s, int x, int y, int w, int h, int r,
                     uint32_t rgb, int a);

/* Separable box blur of a rectangular region, in place — `radius` px, `passes` iterations
 * (3 passes ≈ Gaussian). Used for backdrop "glass" and soft shadows. */
void nw_blur_rect(const struct nw_surface *s, int x, int y, int w, int h, int radius, int passes);

#endif /* NW_GFX_H */
