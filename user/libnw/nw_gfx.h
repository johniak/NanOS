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
};

/* All coordinates may be partially or fully off-surface; everything clips. */
void nw_put_pixel(const struct nw_surface *s, int x, int y, uint32_t rgb);
void nw_fill_rect(const struct nw_surface *s, int x, int y, int w, int h, uint32_t rgb);
void nw_draw_char(const struct nw_surface *s, int x, int y, unsigned char ch,
                  uint32_t fg, uint32_t bg);
/* Draws str left-to-right at 8px advance; no wrapping. Returns the x past the last glyph. */
int  nw_draw_text(const struct nw_surface *s, int x, int y, const char *str,
                  uint32_t fg, uint32_t bg);
/* Copy a w*h block from src(sx,sy) to dst(dx,dy). Clips against BOTH surfaces (negative
 * offsets included) — the compositor's window-into-backbuffer blit. */
void nw_blit(const struct nw_surface *dst, int dx, int dy,
             const struct nw_surface *src, int sx, int sy, int w, int h);

#endif /* NW_GFX_H */
