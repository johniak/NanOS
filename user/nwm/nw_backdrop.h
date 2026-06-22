/*
 * nw_backdrop.h — pure backdrop-blur math for the NanWM compositor's "glass" windows.
 * Rect helpers + box downsample + bilinear upsample, all on 0x00RRGGBB buffers. No I/O,
 * no allocation (every buffer is caller-owned), so the whole module is host-tested.
 *
 * The blur itself reuses nw_blur_rect (nw_gfx.h) on the small downsampled buffer; the
 * upsample reuses nw_lerp. This file adds only what those do not already provide.
 */
#ifndef NW_BACKDROP_H
#define NW_BACKDROP_H

#include <stdint.h>
#include "nw_gfx.h"

/* Tunables (single place; easy A/B in QEMU). */
#define NW_BD_DOWNSAMPLE  4
#define NW_BD_BLUR_RADIUS 24
#define NW_BD_BLUR_PASSES 3
#define NW_BD_FASTDRAG_N  4

/* Screen-space rectangle. w/h >= 0; w==0 || h==0 means empty. */
typedef struct { int x, y, w, h; } nw_rect;

int     nw_rect_empty(nw_rect r);
int     nw_rect_intersects(nw_rect a, nw_rect b);
nw_rect nw_rect_expand(nw_rect r, int by);                 /* grow by `by` on every side */
nw_rect nw_rect_clamp(nw_rect r, int sw, int sh);          /* clamp into [0,sw)x[0,sh) */
nw_rect nw_rect_intersect(nw_rect a, nw_rect b);           /* empty if disjoint */
nw_rect nw_rect_union(nw_rect a, nw_rect b);
/* Window rect expanded by blur_radius then clamped to the screen — the area to sample/blur. */
nw_rect nw_cache_rect(nw_rect win, int blur_radius, int sw, int sh);

/* Box-average downsample of `src` (sw x sh, row stride src_stride px) into `dst`
 * (sw/factor x sh/factor, packed). Each dst pixel is the mean of a factor x factor block;
 * partial edge blocks average only the pixels that exist. factor >= 1. */
void nw_downsample_box(const uint32_t *src, int sw, int sh, int src_stride,
                       uint32_t *dst, int factor);

/* Bilinear upscale of `lo` (lw x lh, packed) into `out` (ow x oh, row stride out_stride px). */
void nw_upsample_bilinear(const uint32_t *lo, int lw, int lh,
                          uint32_t *out, int ow, int oh, int out_stride);

/* 1 if a window's cached blur for `cached` can be reused for the wanted cache_rect `want`
 * (same size and origin, and not flagged dirty); else 0 (must rebuild). */
int nw_backdrop_reusable(nw_rect cached, nw_rect want, int dirty);

/* Largest single axis-aligned sub-rect of `r` not covered by `over` — used to skip blur work
 * under the part of a window hidden by a window above it. Returns `r` if they do not overlap,
 * an empty rect if `over` fully covers `r`, else the bigger of the four border slabs. */
nw_rect nw_rect_visible_band(nw_rect r, nw_rect over);

#endif /* NW_BACKDROP_H */
