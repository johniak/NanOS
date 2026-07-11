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
/* Transparent-background text: the glyph coverage is blended in fg over the backdrop. Uses the
 * proportional anti-aliased UI font (TTF) when loaded, else the 1-bit VGA fallback. Returns end x. */
int  nw_text(const struct nw_surface *s, int x, int y, const char *str, uint32_t fg);
/* Pixel width of a string in the current UI font (proportional). Use for measurement/centering. */
int  nw_text_w(const char *str);
/* Draw one char (transparent bg) in the fixed-cell monospace font (AA) — for text input widgets
 * that keep NW_FONT_W grid math. Caller advances by NW_FONT_W. */
void nw_draw_char_t(const struct nw_surface *s, int x, int y, unsigned char ch, uint32_t fg);
/* Reload the proportional UI font (e.g. a Settings font switch). px<=0 keeps the default size. */
void nw_font_set_ui(const char *path, int px);
/* Re-read settings.yaml (ui_font key) and reload the UI font. Compositor calls this on reload. */
void nw_font_reload_from_settings(void);
/* Copy a w*h block from src(sx,sy) to dst(dx,dy). Clips against BOTH surfaces (negative
 * offsets included) — the compositor's window-into-backbuffer blit. */
void nw_blit(const struct nw_surface *dst, int dx, int dy,
             const struct nw_surface *src, int sx, int sy, int w, int h);

/* ---- modern compositing: alpha, gradients, rounded rects, blur (all clipped + scissored) ---- */

/* src over dst at coverage a (0..255), the per-pixel blend used by every translucent path
 * (window glass, rounded corners, dock). RB-paired: the R and B channels (mask 0x00FF00FF) blend
 * in one multiply, G in another — 2 multiplies per pixel instead of 6 channel-at-a-time. `a` is
 * remapped to 0..256 with `a += a>>7` so the divide is a cheap >>8 yet a==255 yields exactly src
 * and a==0 exactly dst (no ±1 drift at the extremes). Inlined in the header so the compositor's
 * hot loop has no cross-TU call. Result is within ±1 LSB/channel of the old /255 form. */
static inline uint32_t nw_blend8(uint32_t d, uint32_t s, unsigned a)
{
	a += a >> 7;                                  /* 0..255 -> 0..256 (255 -> 256 == exact src) */
	unsigned ia = 256u - a;
	uint32_t rb = (((s & 0x00FF00FFu) * a + (d & 0x00FF00FFu) * ia) >> 8) & 0x00FF00FFu;
	uint32_t g  = (((s & 0x0000FF00u) * a + (d & 0x0000FF00u) * ia) >> 8) & 0x0000FF00u;
	return rb | g;
}

/* Colour math (0x00RRGGBB): src over dst at coverage a (0..255); linear a..b as t/n. */
uint32_t nw_mix(uint32_t dst, uint32_t src, int a);
uint32_t nw_lerp(uint32_t a, uint32_t b, int t, int n);

/* Alpha-blend rgb over the surface at coverage a (0..255). Per-pixel and filled-rect forms. */
void nw_blend_pixel(const struct nw_surface *s, int x, int y, uint32_t rgb, int a);
void nw_blend_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t rgb, int a);

/* Glyph coverage -> alpha remap (gamma 1.43, the Photoshop/Skia compromise): raw stb_truetype
 * AA coverage blended directly in sRGB renders dark text thin. Returns a lazily-initialized
 * 256-entry LUT; index by coverage (0..255) before calling nw_blend_pixel for GLYPH AA only —
 * never for geometric AA (nw_fill_round/nw_stroke_round) or real alpha (icon PNGs). */
const uint8_t *nw_cov143(void);

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

/* ---- straight-alpha ARGB ink primitives (WRITE the alpha byte) --------------------------------
 * Every primitive above masks alpha out (0x00RRGGBB dest, straight RGB fill/blend). Glass
 * interiors and the alpha-ink titlebar need the inverse: dest pixels that carry real coverage in
 * their own alpha byte, straight (non-premultiplied) — the GL shader contract is client buffer =
 * straight ARGB, ctex.a = ink coverage, ctex.rgb = ink colour at full strength. `argb` here is
 * always 0xAARRGGBB (alpha in the top byte), unlike the masked 0x00RRGGBB used above. */
#include "nw_over_core.h"   /* nw_over_pixel: static inline, shared with the host blend test */

/* Raw store (like nw_fill_rect, but the full 32-bit value including alpha — no blending). */
void nw_clear_argb(const struct nw_surface *s, int x, int y, int w, int h, uint32_t argb);
/* Filled straight-alpha src-over rect: nw_over_pixel over every covered pixel. */
void nw_over_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t argb);
/* Rounded-rect straight-alpha src-over fill, AA corners (same corner geometry as nw_fill_round). */
void nw_over_round(const struct nw_surface *s, int x, int y, int w, int h, int r, uint32_t argb);
/* Straight-alpha text: glyph coverage (cov143-remapped) scaled by argb's own alpha, written via
 * nw_over_pixel. Same font/baseline/advance as nw_text; VGA 1-bit fallback if no TTF loaded. */
void nw_text_argb(const struct nw_surface *s, int x, int y, const char *str, uint32_t argb);
/* Pixel width — identical to nw_text_w (measurement doesn't depend on alpha). */
int  nw_text_argb_w(const char *str);

#endif /* NW_GFX_H */
