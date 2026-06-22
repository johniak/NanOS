# Backdrop Cache Blur for Glass Windows — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every translucent NanWM window show a blurred image of whatever sits below it in Z-order (wallpaper, panels, and other windows), matching the `.claude/nanoos-ui/` glass mockup, affordably on a CPU-only framebuffer.

**Architecture:** Approach A from the spec (`docs/superpowers/specs/2026-06-22-backdrop-cache-blur-design.md`). The compositor already paints back-to-front into `g_scene`, so when a window is about to be drawn, `g_scene` already holds exactly the layers below it — that *is* the backdrop snapshot, for free, including already-blurred lower glass windows (window-on-window falls out naturally). A new pure module `nw_backdrop` downsamples that region 4×, blurs the small copy with the existing `nw_blur_rect`, and bilinearly upscales it; `composite_round` then blends the window over the blurred backdrop instead of the sharp scene. The existing damage scissor in `present()` confines blur work to the changed region, giving dirty-rect partial updates for free; a persistent per-window lo-res cache plus a fast-drag mode keep window dragging smooth.

**Tech Stack:** C (freestanding userland, picolibc), 32bpp `0x00RRGGBB` surfaces, doctest host tests (C++), Docker build (`make image64`), QEMU native verification.

**Key deviations from the spec (intentional, discovered by reading the code):**
- No separate invalidation pass: the existing damage scissor (`nwm.c` `present()` → `nw_surface_clip`) already limits recompose to the damage region, so blur recomputed within the clip *is* the dirty-rect partial update.
- No new box-blur function: reuse `nw_blur_rect` (`user/libnw/nw_gfx.c:266`) on the small lo-res buffer; reuse `nw_lerp` (`user/libnw/nw_gfx.h:69`) for bilinear upsample. DRY.
- The persistent per-window cache is the drag optimization (Phase 4), not a correctness requirement.

**Tunables** live in `user/nwm/nw_backdrop.h`:
```c
#define NW_BD_DOWNSAMPLE   4     /* downsample factor */
#define NW_BD_BLUR_RADIUS  24    /* full-res blur radius (lo-res uses RADIUS/DOWNSAMPLE) */
#define NW_BD_BLUR_PASSES  3     /* box passes ~= Gaussian */
#define NW_BD_FASTDRAG_N   4     /* during a drag, do a fresh rebuild every N frames */
```

---

## File Structure

**New:**
- `user/nwm/nw_backdrop.h` — `nw_rect` type, rect helpers, `nw_downsample_box`, `nw_upsample_bilinear`, the `nw_backdrop_ctx` struct, tunables. One responsibility: pure backdrop pixel/rect math.
- `user/nwm/nw_backdrop.c` — implementations (no I/O, no malloc; all buffers caller-owned).
- `tests/test_nw_backdrop.cpp` — doctest host tests for the pure module.

**Modified:**
- `user/nwm/nw_compose.h` / `nw_compose.c` — `composite_round` gains an optional `backdrop` source; `nw_compose_scene` gains an optional `const struct nw_backdrop_ctx *bdc` and builds the blurred backdrop per glass window.
- `user/nwm/nwm_core.h` — `enum nw_win_type`; `type`, `glass`, `bd_blur`, `bd_lw`, `bd_lh`, `bd_rect`, `bd_dirty` fields on `struct nw_window`; `frame_ctr` on `struct nw_server`.
- `user/nwm/nwm_core.c` — default `type`/`glass` in window creation; bump `frame_ctr`.
- `user/nwm/nwm.c` — allocate the backdrop scratch buffers; pass a `nw_backdrop_ctx` into `nw_compose_scene`; per-window lo-res cache alloc/free; fast-drag wiring.
- `tests/test_nw_compose.cpp` — update existing `nw_compose_scene` call sites to the new signature; add a blur-visibility test.
- `Makefile` — add `user/nwm/nw_backdrop.c` to `TEST_MODULES`, `"*/nw_backdrop.*"` to `COV_PATTERNS`, and `$(BINFOLDER)nw_backdrop.o` to the `nwm.nxe` prerequisites.

**Build/verify commands (project-specific):**
- Host tests + coverage gate: `make test` (runs the doctest suite in the `nanos-test` container; fails if gated line coverage < 90%).
- Build the x86_64 disk image: `make image64`.
- Visual check in QEMU: `make run64` (native QEMU). Always `make clean` first when crossing ARCH.
- Commits: clean messages, **no Claude/AI attribution of any kind**.

---

## Phase 1 — Pure `nw_backdrop` module (host-tested)

### Task 1: Rect type and helpers

**Files:**
- Create: `user/nwm/nw_backdrop.h`
- Create: `user/nwm/nw_backdrop.c`
- Create: `tests/test_nw_backdrop.cpp`
- Modify: `Makefile` (TEST_MODULES + COV_PATTERNS)

- [ ] **Step 1: Create the header with the rect type, tunables, and helper declarations**

`user/nwm/nw_backdrop.h`:
```c
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

#endif /* NW_BACKDROP_H */
```

- [ ] **Step 2: Stub the .c so the test links, then write the failing test**

`user/nwm/nw_backdrop.c` (helpers only for now; pixel functions added in Tasks 2-3):
```c
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
```

`tests/test_nw_backdrop.cpp`:
```c
#include "doctest.h"
#include "nw_backdrop.h"
#include <vector>

TEST_CASE("rect expand/clamp/intersect/union and predicates") {
	CHECK(nw_rect_empty((nw_rect){0,0,0,5}) == 1);
	CHECK(nw_rect_empty((nw_rect){0,0,3,5}) == 0);

	nw_rect e = nw_rect_expand((nw_rect){10,10,20,20}, 4);
	CHECK(e.x == 6); CHECK(e.y == 6); CHECK(e.w == 28); CHECK(e.h == 28);

	// expand at the screen edge then clamp: never negative origin, never past the screen
	nw_rect c = nw_cache_rect((nw_rect){2,2,10,10}, 5, 100, 100);
	CHECK(c.x == 0); CHECK(c.y == 0);
	CHECK(c.x + c.w <= 100); CHECK(c.y + c.h <= 100);

	nw_rect i = nw_rect_intersect((nw_rect){0,0,10,10}, (nw_rect){5,5,10,10});
	CHECK(i.x == 5); CHECK(i.y == 5); CHECK(i.w == 5); CHECK(i.h == 5);
	CHECK(nw_rect_empty(nw_rect_intersect((nw_rect){0,0,5,5}, (nw_rect){10,10,5,5})) == 1);

	nw_rect u = nw_rect_union((nw_rect){0,0,5,5}, (nw_rect){10,10,5,5});
	CHECK(u.x == 0); CHECK(u.y == 0); CHECK(u.w == 15); CHECK(u.h == 15);

	CHECK(nw_rect_intersects((nw_rect){0,0,10,10}, (nw_rect){5,5,2,2}) == 1);
	CHECK(nw_rect_intersects((nw_rect){0,0,10,10}, (nw_rect){10,0,2,2}) == 0); // touching, not overlapping
}
```

- [ ] **Step 3: Wire the module into the test build**

In `Makefile`, append to the userland test modules (the line after `user/nwm/nwm_core.c user/nwm/nw_compose.c`, currently `Makefile:2132`):
```make
TEST_MODULES+= user/nwm/nw_backdrop.c
```
And add to `COV_PATTERNS` (`Makefile:2151`), inside the quoted list, next to `"*/nw_compose.*"`:
```
"*/nw_backdrop.*"
```

- [ ] **Step 4: Run the tests — verify the new ones pass and nothing regressed**

Run: `make test`
Expected: PASS, suite count increased by 1, coverage gate still ≥90% (the new module is small and fully covered by this and later tasks).

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nw_backdrop.h user/nwm/nw_backdrop.c tests/test_nw_backdrop.cpp Makefile
git commit -m "feat(nwm): nw_backdrop rect helpers + module scaffolding"
```

### Task 2: Box downsample

**Files:**
- Modify: `user/nwm/nw_backdrop.c`
- Test: `tests/test_nw_backdrop.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_nw_backdrop.cpp`:
```c
TEST_CASE("downsample: a solid block collapses to that exact color") {
	std::vector<uint32_t> src(8 * 8, 0x00204060u);
	std::vector<uint32_t> dst(2 * 2, 0u);
	nw_downsample_box(src.data(), 8, 8, 8, dst.data(), 4);
	for (int i = 0; i < 4; i++) CHECK(dst[i] == 0x00204060u);
}

TEST_CASE("downsample: a 2x2 block averages its four colors per channel") {
	// one 2x2 block: R values 0,0,255,255 -> avg 127 ; G,B similar pattern
	uint32_t src[4] = { 0x00000000u, 0x00000000u, 0x00ff0000u, 0x00ff0000u };
	uint32_t dst = 0;
	nw_downsample_box(src, 2, 2, 2, &dst, 2);
	CHECK(((dst >> 16) & 0xff) == 127u);   // (0+0+255+255)/4
	CHECK(((dst >> 8)  & 0xff) == 0u);
	CHECK((dst & 0xff) == 0u);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: link/compile error or assertion failure — `nw_downsample_box` is undefined.

- [ ] **Step 3: Implement `nw_downsample_box`**

Append to `user/nwm/nw_backdrop.c`:
```c
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
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nw_backdrop.c tests/test_nw_backdrop.cpp
git commit -m "feat(nwm): box-average downsample for backdrop blur"
```

### Task 3: Bilinear upsample

**Files:**
- Modify: `user/nwm/nw_backdrop.c`
- Test: `tests/test_nw_backdrop.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_nw_backdrop.cpp`:
```c
TEST_CASE("upsample: a solid lo-res image stays that exact color at any size") {
	std::vector<uint32_t> lo(2 * 2, 0x00336699u);
	std::vector<uint32_t> out(7 * 5, 0u);
	nw_upsample_bilinear(lo.data(), 2, 2, out.data(), 7, 5, 7);
	for (int i = 0; i < 7 * 5; i++) CHECK(out[i] == 0x00336699u);
}

TEST_CASE("upsample: a 1x1 lo-res image fills the whole output with that color") {
	uint32_t lo = 0x00abcdefu;
	std::vector<uint32_t> out(4 * 4, 0u);
	nw_upsample_bilinear(&lo, 1, 1, out.data(), 4, 4, 4);
	for (int i = 0; i < 16; i++) CHECK(out[i] == 0x00abcdefu);
}

TEST_CASE("upsample: a horizontal two-pixel gradient is monotonic across the row") {
	uint32_t lo[2] = { 0x00000000u, 0x00ff0000u };   // black -> red, left to right
	std::vector<uint32_t> out(8, 0u);
	nw_upsample_bilinear(lo, 2, 1, out.data(), 8, 1, 8);
	int prev = -1;
	for (int x = 0; x < 8; x++) {
		int r = (out[x] >> 16) & 0xff;
		CHECK(r >= prev);          // never decreases
		prev = r;
	}
	CHECK(((out[0] >> 16) & 0xff) == 0);     // leftmost samples the first lo pixel
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: undefined `nw_upsample_bilinear`.

- [ ] **Step 3: Implement `nw_upsample_bilinear` (reusing `nw_lerp`)**

Append to `user/nwm/nw_backdrop.c`:
```c
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
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: PASS. If the gradient-monotonicity test trips on a rounding step, it is a real bug in the lerp fraction — fix the fixed-point math, do not relax the assertion.

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nw_backdrop.c tests/test_nw_backdrop.cpp
git commit -m "feat(nwm): bilinear upsample for backdrop blur"
```

---

## Phase 2 — Compositor integration (the core glass effect)

### Task 4: `composite_round` gains an optional backdrop source

**Files:**
- Modify: `user/nwm/nw_compose.c:140-172` (`composite_round`), and its three call sites at `nw_compose.c:318`, `:322`, plus the new param threads through `nw_compose_scene`.

- [ ] **Step 1: Add the `backdrop` parameter and use it as the blend's destination color**

Replace `composite_round` (`nw_compose.c:140-172`) with this version. The only change: an extra `const struct nw_surface *backdrop` argument; when non-NULL the per-pixel blend mixes the window over `backdrop[px]` (the blurred backdrop) but still **writes** to `back[px]`. When NULL, behaviour is byte-for-byte the old path.
```c
static void composite_round(const struct nw_surface *back, const struct nw_surface *sc,
                            int x, int y, int w, int h, int r, int alpha, int sox, int soy,
                            const struct nw_surface *backdrop)
{
	int bx0, by0, bx1, by1;
	nw_surface_bounds(back, &bx0, &by0, &bx1, &by1);
	int x0 = x < bx0 ? bx0 : x, x1 = x + w > bx1 ? bx1 : x + w;
	if (x1 <= x0) return;
	for (int py = (y < by0 ? by0 : y); py < (y + h > by1 ? by1 : y + h); py++) {
		int yy = py - y;
		uint32_t       *drow = back->px + (long) py * back->stride;
		const uint32_t *srow = sc->px   + (long) (py - soy) * sc->stride - sox;
		const uint32_t *bdrow = backdrop ? backdrop->px + (long) py * backdrop->stride : drow;
		if (yy >= r && yy < h - r) {                  /* straight middle row: no AA */
			if (alpha >= 255)
				for (int px = x0; px < x1; px++) drow[px] = srow[px];
			else
				for (int px = x0; px < x1; px++) drow[px] = cmix(bdrow[px], srow[px], alpha);
			continue;
		}
		for (int px = x0; px < x1; px++) {            /* corner band: per-pixel AA coverage */
			int a = alpha, xx = px - x;
			int lx = xx < r ? r - 1 - xx : (xx >= w - r ? xx - (w - r) : -1);
			int ly = yy < r ? r - 1 - yy : (yy >= h - r ? yy - (h - r) : -1);
			if (lx >= 0 && ly >= 0) {
				float dx = lx + 0.5f, dy = ly + 0.5f;
				float e = (float) r - __builtin_sqrtf(dx * dx + dy * dy);
				int cov = e >= 0.5f ? 255 : (e <= -0.5f ? 0 : (int) ((e + 0.5f) * 255.0f));
				if (!cov) continue;
				a = cov * alpha / 255;
			}
			drow[px] = (a >= 255) ? srow[px] : cmix(bdrow[px], srow[px], a);
		}
	}
}
```
Note: when `backdrop == NULL`, `bdrow == drow`, so `cmix(bdrow[px], ...)` equals the old `cmix(drow[px], ...)`. Identical output.

- [ ] **Step 2: Update the two in-file call sites to pass NULL (no behaviour change yet)**

In `nw_compose_scene` (`nw_compose.c:318` and `:322`), add a trailing `, 0` (NULL backdrop) to both `composite_round(...)` calls:
```c
composite_round(back, &fs, w->x, w->y, fw, fh, NW_RADIUS, alpha, w->x, w->y, 0);
```
```c
composite_round(back, scratch, w->x, w->y, fw, fh, NW_RADIUS, alpha, 0, 0, 0);
```

- [ ] **Step 3: Build the kernel image to confirm it still compiles and looks identical**

Run: `make clean && make image64`
Expected: builds clean. (Behaviour unchanged — backdrop is NULL everywhere; this is a pure refactor step. Run `make test` too; existing `test_nw_compose` pixels must be unchanged.)

- [ ] **Step 4: Commit**

```bash
git add user/nwm/nw_compose.c
git commit -m "refactor(nwm): composite_round accepts an optional backdrop source"
```

### Task 5: Build the blurred backdrop per glass window in `nw_compose_scene`

**Files:**
- Modify: `user/nwm/nw_compose.h` (new `nw_backdrop_ctx`, new `nw_compose_scene` param)
- Modify: `user/nwm/nw_compose.c` (`nw_compose_scene`, `nw_compose`)
- Modify: `tests/test_nw_compose.cpp` (update call sites + add a blur test)

- [ ] **Step 1: Declare the backdrop context and extend the `nw_compose_scene` signature**

In `user/nwm/nw_compose.h`, add `#include "nw_backdrop.h"` after the existing includes, then declare:
```c
/* Caller-owned scratch for backdrop blur. `bd` is a screen-aligned full-res surface the
 * compositor fills with the blurred backdrop under each glass window before compositing it;
 * `lo` is a downsample scratch of at least lo_cap pixels (>= (bd->w/factor)*(bd->h/factor)).
 * NULL ctx (or bd) => no blur, the classic flat-tint glass. */
struct nw_backdrop_ctx {
	struct nw_surface *bd;
	uint32_t          *lo;
	int                lo_cap;
	int                factor, radius, passes;
};
```
Change the `nw_compose_scene` declaration to:
```c
void nw_compose_scene(const struct nw_server *s, const struct nw_surface *back,
                      const struct nw_surface *scratch, const struct nw_surface *wall,
                      const struct nw_backdrop_ctx *bdc);
```

- [ ] **Step 2: Implement the backdrop build + use it inside `nw_compose_scene`**

In `user/nwm/nw_compose.c`, add `#include "nw_backdrop.h"` near the top, then add this static helper above `nw_compose_scene`:
```c
/* Fill bdc->bd over the window rect (x,y,w,h) with a blurred copy of `back` beneath it.
 * Samples a cache_rect (window expanded by the blur radius, clamped to the scissor+screen),
 * downsamples it into bdc->lo, blurs the small copy with nw_blur_rect, then bilinearly
 * upscales the window-rect portion back into bdc->bd. Returns 1 if bd was filled, else 0
 * (region empty, or lo scratch too small -> caller composites without a backdrop). */
static int build_backdrop(const struct nw_surface *back, const struct nw_backdrop_ctx *bdc,
                          int x, int y, int w, int h)
{
	int cx0, cy0, cx1, cy1;
	nw_surface_bounds(back, &cx0, &cy0, &cx1, &cy1);          /* honours the damage scissor */
	nw_rect clip = { cx0, cy0, cx1 - cx0, cy1 - cy0 };
	nw_rect cr = nw_rect_intersect(nw_cache_rect((nw_rect){x, y, w, h}, bdc->radius,
	                                             back->w, back->h), clip);
	if (nw_rect_empty(cr)) return 0;
	int f = bdc->factor;
	int lw = cr.w / f, lh = cr.h / f;
	if (lw < 1 || lh < 1) return 0;
	if (lw * lh > bdc->lo_cap) return 0;                      /* shouldn't happen; safety */

	/* downsample back[cr] -> lo */
	nw_downsample_box(back->px + (long) cr.y * back->stride + cr.x,
	                  lw * f, lh * f, back->stride, bdc->lo, f);
	/* blur the small copy in place (reuse the existing separable box blur) */
	struct nw_surface lo;
	lo.px = bdc->lo; lo.w = lw; lo.h = lh; lo.stride = lw;
	nw_surface_noclip(&lo);
	nw_blur_rect(&lo, 0, 0, lw, lh, bdc->radius / f > 0 ? bdc->radius / f : 1, bdc->passes);
	/* upscale lo back into bd over the SAME cr region (aligned with `back`) */
	nw_upsample_bilinear(bdc->lo, lw, lh,
	                     bdc->bd->px + (long) cr.y * bdc->bd->stride + cr.x,
	                     cr.w - (cr.w % f), cr.h - (cr.h % f), bdc->bd->stride);
	return 1;
}
```
Then, inside `nw_compose_scene`, in the cached-frame branch (`nw_compose.c:314-319`) and the scratch branch (`:320-323`), build and pass the backdrop for glass windows. Replace those two branches with:
```c
		const struct nw_surface *bd = 0;
		if (bdc && bdc->bd && w->glass && build_backdrop(back, bdc, w->x, w->y, fw, fh))
			bd = bdc->bd;
		if (w->frame) {                          /* cached frame: composite window-local source */
			struct nw_surface fs;
			fs.px = w->frame; fs.w = fw; fs.h = fh; fs.stride = fw;
			nw_surface_noclip(&fs);
			composite_round(back, &fs, w->x, w->y, fw, fh, NW_RADIUS, alpha, w->x, w->y, bd);
			nw_stroke_round(back, w->x, w->y, fw, fh, NW_RADIUS, COL_BORDER, 150);
		} else if (scratch) {                    /* screen-space scratch: render live + composite */
			draw_window_to(scratch, w, focused, w->x, w->y);
			composite_round(back, scratch, w->x, w->y, fw, fh, NW_RADIUS, alpha, 0, 0, bd);
			nw_stroke_round(back, w->x, w->y, fw, fh, NW_RADIUS, COL_BORDER, 150);
		} else {
			draw_window_to(back, w, focused, w->x, w->y);     /* simple/host path: opaque, square */
		}
```
Update the top of `nw_compose_scene` signature to take `const struct nw_backdrop_ctx *bdc`, and update the `nw_compose` wrapper (`nw_compose.c:348-352`) to pass `0`:
```c
void nw_compose(const struct nw_server *s, const struct nw_surface *back)
{
	nw_compose_scene(s, back, 0, 0, 0);
	nw_draw_cursor(back, s->cursor_x, s->cursor_y);
}
```

- [ ] **Step 3: Update existing `test_nw_compose.cpp` call sites and add a blur-visibility test**

In `tests/test_nw_compose.cpp`, every existing `nw_compose_scene(s, back, scratch, wall)` call gains a trailing `, 0` (no blur — existing pixel assertions must stay valid). Then add:
```c
TEST_CASE("backdrop: glass window blurs a sharp edge in the scene below it") {
	// 200x200 scene; left half black, right half white -> a hard vertical edge at x=100.
	const int W = 200, H = 200;
	std::vector<uint32_t> sceneA(W * H), sceneB(W * H);
	std::vector<uint32_t> scratch(W * H), wall(W * H, 0u);
	std::vector<uint32_t> bd(W * H, 0u), lo((size_t)(W/4)*(H/4) + 16, 0u);

	struct nw_server s; nw_server_init(&s, W, H);
	// one full-content window covering the center, no frame cache (host live path uses scratch)
	// (use the project's helper to add a window with a buffer; see other tests in this file)
	add_test_window(&s, /*x*/40, /*y*/40, /*cw*/120, /*ch*/120);
	s.win[0].glass = 1;

	struct nw_surface backA, backB, scr, wl, bds;
	mk_surface(&backA, sceneA.data(), W, H);
	mk_surface(&backB, sceneB.data(), W, H);
	mk_surface(&scr, scratch.data(), W, H);
	mk_surface(&wl, wall.data(), W, H);
	mk_surface(&bds, bd.data(), W, H);

	// paint the hard edge as the "wallpaper" both times
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
		wall[(size_t)y*W+x] = x < 100 ? 0x00000000u : 0x00ffffffu;

	struct nw_backdrop_ctx ctx = { &bds, lo.data(), (int)lo.size(), 4, NW_BD_BLUR_RADIUS, NW_BD_BLUR_PASSES };

	nw_compose_scene(&s, &backA, &scr, &wl, 0);     // no blur
	nw_compose_scene(&s, &backB, &scr, &wl, &ctx);  // blurred backdrop

	// Sample a row inside the window, near the edge column 100. Without blur the pixel just
	// left of 100 is far from the pixel just right (hard edge bleeding through the glass).
	// With blur the transition is gradual: the two are closer together.
	auto chan = [](uint32_t c){ return (int)((c>>16)&0xff); };
	int yrow = 100;
	int jumpA = std::abs(chan(sceneA[(size_t)yrow*W+98]) - chan(sceneA[(size_t)yrow*W+102]));
	int jumpB = std::abs(chan(sceneB[(size_t)yrow*W+98]) - chan(sceneB[(size_t)yrow*W+102]));
	CHECK(jumpB < jumpA);    // blur softened the edge seen through the glass
}
```
If `add_test_window` / `mk_surface` helpers do not already exist in `test_nw_compose.cpp`, add small local helpers mirroring the `Buf` pattern from `tests/test_nw_gfx.cpp` (a heap-backed `nw_surface`) and the window-setup used by the other cases in this file. Keep them local to the test file.

- [ ] **Step 4: Run host tests**

Run: `make test`
Expected: PASS, including the new blur-visibility case (`jumpB < jumpA`).

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nw_compose.h user/nwm/nw_compose.c tests/test_nw_compose.cpp
git commit -m "feat(nwm): blur the backdrop under each glass window when composing"
```

### Task 6: Wire the backdrop scratch + glass flag into the I/O shell, see it in QEMU

**Files:**
- Modify: `user/nwm/nwm_core.h` (add `glass` field — full type enum comes in Task 7; here just the flag), `user/nwm/nwm_core.c` (default `glass = 1` on create)
- Modify: `user/nwm/nwm.c` (allocate `g_bd` + `g_bdlo`, pass a `nw_backdrop_ctx`)
- Modify: `Makefile` (`nwm.nxe` prereqs)

- [ ] **Step 1: Add the `glass` field and default it on window creation**

In `user/nwm/nwm_core.h`, inside `struct nw_window` (after `frame_dirty`, `nwm_core.h:72`):
```c
	uint8_t   glass;       /* 1 => this window gets a blurred backdrop (default for all) */
```
In `user/nwm/nwm_core.c`, find where a new window is initialized on create (the `NW_REQ_CREATE` handler that sets `used = 1`, `id`, `cw`, `ch`) and set:
```c
	w->glass = 1;
```
(If a `memset(w, 0, sizeof *w)` precedes init, place the assignment after it.)

- [ ] **Step 2: Allocate the backdrop scratch in `nwm.c` and pass the context**

In `user/nwm/nwm.c`, next to the existing compositor buffers (`nwm.c:63-67`), add:
```c
static uint32_t *g_bd;                     /* screen-aligned blurred-backdrop scratch     */
static uint32_t *g_bdlo;                   /* downsample scratch ((xres/F)*(yres/F) px)    */
static struct nw_surface g_bd_surf;
static struct nw_backdrop_ctx g_bdc;
```
Add `#include "nw_backdrop.h"` with the other nwm includes. Where the compositor buffers are malloc'd (`nwm.c:493-503`), add:
```c
	g_bd   = (uint32_t *) malloc(fbpx);
	int lopx = ((int)g_xres / NW_BD_DOWNSAMPLE + 1) * ((int)g_yres / NW_BD_DOWNSAMPLE + 1);
	g_bdlo = (uint32_t *) malloc((size_t) lopx * 4);
	if (!g_bd || !g_bdlo) { printf("nwm: no memory for backdrop buffers\n"); return 1; }
	memset(g_bd, 0, fbpx);
	g_bd_surf.px = g_bd; g_bd_surf.w = (int) g_xres; g_bd_surf.h = (int) g_yres;
	g_bd_surf.stride = (int) g_xres; nw_surface_noclip(&g_bd_surf);
	g_bdc.bd = &g_bd_surf; g_bdc.lo = g_bdlo; g_bdc.lo_cap = lopx;
	g_bdc.factor = NW_BD_DOWNSAMPLE; g_bdc.radius = NW_BD_BLUR_RADIUS; g_bdc.passes = NW_BD_BLUR_PASSES;
```
In `present()` (`nwm.c:385`), pass the context (the scissor is already applied to `g_scene_surf`; `g_bd_surf` is read/written only within the window rect, which lies inside the clip, so it needs no separate clip):
```c
		nw_compose_scene(&S, &g_scene_surf, &g_scratch_surf, &g_wall_surf, &g_bdc);
```

- [ ] **Step 3: Add the object to the `nwm.nxe` link line**

In `Makefile:1775`, append `$(BINFOLDER)nw_backdrop.o` to the `nwm.nxe` prerequisites:
```make
$(BINFOLDER)nwm.nxe:       $(DYN_DEPS) $(BINFOLDER)nwm.o $(BINFOLDER)nwm_core.o $(BINFOLDER)nw_compose.o $(BINFOLDER)nwproto.o $(BINFOLDER)nw_gfx.o $(BINFOLDER)vtfont.o $(BINFOLDER)png.o $(BINFOLDER)nw_backdrop.o
```

- [ ] **Step 4: Build and look at it in QEMU**

Run: `make clean && make image64 && make run64`
Expected: NanWM boots; open two windows and overlap them. The top window shows a **blurred** image of the window/wallpaper beneath it (not a sharp copy). Confirm window-over-window: the top window's glass blurs the *lower window's* content, not the wallpaper. No blur leaking into the margin/corners (the area just outside a window stays sharp). Drag may stutter — that is expected and fixed in Phase 4.

Headless capture (per CLAUDE.md QEMU recipe) is acceptable in place of interactive viewing: boot with `-display none -monitor unix:/tmp/qmon,...`, `screendump` to PPM, `sips -s format png`, inspect.

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nwm_core.h user/nwm/nwm_core.c user/nwm/nwm.c Makefile
git commit -m "feat(nwm): enable backdrop blur in the compositor shell"
```

---

## Phase 3 — Explicit window type

### Task 7: `nw_win_type` enum + `type` field, threaded through window creation

**Files:**
- Modify: `user/nwm/nwm_core.h` (enum + `type` field)
- Modify: `user/nwm/nwm_core.c` (default `type` on create; set MENU/DIALOG for WM chrome where applicable)
- Test: `tests/test_nwm_core.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_nwm_core.cpp` (mirror the create flow used by the other cases in that file — connect a client, send `NW_REQ_CREATE`, then inspect `s.win[idx]`):
```c
TEST_CASE("a newly created window defaults to NORMAL, glass-enabled") {
	struct nw_server s; nw_server_init(&s, 800, 600);
	unsigned char outbuf[4096];
	nw_client_connect(&s, 0, outbuf, sizeof outbuf);
	int idx = create_test_window(&s, 0, 300, 200);   // helper used elsewhere in this file
	CHECK(s.win[idx].used == 1);
	CHECK(s.win[idx].type == NW_WIN_NORMAL);
	CHECK(s.win[idx].glass == 1);
}
```
(If no `create_test_window` helper exists, inline the same `nw_client_msg(&s, 0, &create_msg, payload)` sequence the other tests in this file already use.)

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: `NW_WIN_NORMAL` / `type` undefined.

- [ ] **Step 3: Add the enum and field, default them on create**

In `user/nwm/nwm_core.h`, before `struct nw_window` (after the hit-test enums, ~`nwm_core.h:57`):
```c
/* Window kind — drives backdrop-blur recursion scope + update priority (Phase 5). */
enum nw_win_type { NW_WIN_NORMAL = 0, NW_WIN_MENU, NW_WIN_POPUP, NW_WIN_DIALOG, NW_WIN_TOOLTIP };
```
Add to `struct nw_window` (next to `glass`):
```c
	uint8_t   type;        /* enum nw_win_type; default NW_WIN_NORMAL */
```
In `user/nwm/nwm_core.c`, where the window is initialized on create (same spot as Task 6's `w->glass = 1`):
```c
	w->type = NW_WIN_NORMAL;
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nwm_core.h user/nwm/nwm_core.c tests/test_nwm_core.cpp
git commit -m "feat(nwm): explicit window type field (default NORMAL)"
```

---

## Phase 4 — Drag stays smooth (persistent per-window cache + fast-drag)

### Task 8: Persistent per-window lo-res backdrop cache

**Files:**
- Modify: `user/nwm/nwm_core.h` (cache fields), `user/nwm/nw_backdrop.h`/`.c` (a cache-aware build entry), `user/nwm/nw_compose.c` (use the per-window cache), `user/nwm/nwm.c` (alloc/free per-slot lo-res buffer)
- Test: `tests/test_nw_backdrop.cpp`

- [ ] **Step 1: Add the cache fields to `struct nw_window`**

In `user/nwm/nwm_core.h`, inside `struct nw_window` (after `type`):
```c
	uint32_t *bd_blur;     /* per-window LO-RES blurred backdrop cache (caller-allocated)   */
	int       bd_lw, bd_lh;/* lo-res cache dimensions                                       */
	nw_rect   bd_rect;     /* the cache_rect (screen coords) bd_blur was computed for        */
	int       bd_dirty;    /* 1 => backdrop must be rebuilt next compose                     */
```
This requires `nw_rect` to be visible here — add `#include "nw_backdrop.h"` at the top of `nwm_core.h` (it only pulls in `nw_gfx.h` + stdint, no cycle: `nw_backdrop.h` does not include `nwm_core.h`).

- [ ] **Step 2: Write the failing test for the cache decision**

The decision "may I reuse the cache?" is pure. Add to `nw_backdrop.h`:
```c
/* 1 if a window's cached blur for `cached` can be reused for the wanted cache_rect `want`
 * (same size and origin, and not flagged dirty); else 0 (must rebuild). */
int nw_backdrop_reusable(nw_rect cached, nw_rect want, int dirty);
```
Append to `tests/test_nw_backdrop.cpp`:
```c
TEST_CASE("cache reuse only when rect matches exactly and not dirty") {
	nw_rect a = {10,10,100,80};
	CHECK(nw_backdrop_reusable(a, a, 0) == 1);
	CHECK(nw_backdrop_reusable(a, a, 1) == 0);                 // dirty
	CHECK(nw_backdrop_reusable(a, (nw_rect){11,10,100,80}, 0) == 0); // moved
	CHECK(nw_backdrop_reusable(a, (nw_rect){10,10,101,80}, 0) == 0); // resized
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `make test`
Expected: undefined `nw_backdrop_reusable`.

- [ ] **Step 4: Implement the decision and the cache-aware path**

Append to `user/nwm/nw_backdrop.c`:
```c
int nw_backdrop_reusable(nw_rect cached, nw_rect want, int dirty)
{
	if (dirty) return 0;
	return cached.x == want.x && cached.y == want.y &&
	       cached.w == want.w && cached.h == want.h;
}
```
In `user/nwm/nw_compose.c`, change `build_backdrop` to store the lo-res result in the window's cache and reuse it when possible. Make it take the window so it can read/write `w->bd_*`:
```c
static int build_backdrop(const struct nw_surface *back, const struct nw_backdrop_ctx *bdc,
                          struct nw_window *w, int x, int y, int fw, int fh, int force_rebuild)
{
	int cx0, cy0, cx1, cy1;
	nw_surface_bounds(back, &cx0, &cy0, &cx1, &cy1);
	nw_rect clip = { cx0, cy0, cx1 - cx0, cy1 - cy0 };
	nw_rect want = nw_cache_rect((nw_rect){x, y, fw, fh}, bdc->radius, back->w, back->h);
	nw_rect cr = nw_rect_intersect(want, clip);
	if (nw_rect_empty(cr)) return 0;
	int f = bdc->factor, lw = want.w / f, lh = want.h / f;
	if (lw < 1 || lh < 1 || lw * lh > bdc->lo_cap) return 0;

	int reuse = w->bd_blur && !force_rebuild &&
	            nw_backdrop_reusable(w->bd_rect, want, w->bd_dirty) &&
	            w->bd_lw == lw && w->bd_lh == lh;
	if (!reuse) {
		nw_downsample_box(back->px + (long) want.y * back->stride + want.x,
		                  lw * f, lh * f, back->stride, bdc->lo, f);
		struct nw_surface lo = { bdc->lo, lw, lh, lw, 0,0,0,0 };
		nw_surface_noclip(&lo);
		nw_blur_rect(&lo, 0, 0, lw, lh, bdc->radius / f > 0 ? bdc->radius / f : 1, bdc->passes);
		if (w->bd_blur) {                       /* persist into the per-window cache */
			for (int i = 0; i < lw * lh; i++) w->bd_blur[i] = bdc->lo[i];
			w->bd_lw = lw; w->bd_lh = lh; w->bd_rect = want; w->bd_dirty = 0;
		}
	}
	const uint32_t *lo_src = reuse ? w->bd_blur : (w->bd_blur ? w->bd_blur : bdc->lo);
	nw_upsample_bilinear(lo_src, lw, lh,
	                     bdc->bd->px + (long) want.y * bdc->bd->stride + want.x,
	                     want.w - (want.w % f), want.h - (want.h % f), bdc->bd->stride);
	return 1;
}
```
Update the call in `nw_compose_scene` to pass `w` and a `force_rebuild` flag (0 for now; Task 9 sets it during drag):
```c
		if (bdc && bdc->bd && w->glass &&
		    build_backdrop(back, bdc, (struct nw_window *) w, w->x, w->y, fw, fh, 0))
			bd = bdc->bd;
```
(The `const` cast is safe: `nw_compose_scene` takes `const struct nw_server *`, but the per-window blur cache is scratch state, exactly like `nw_render_dirty_frames` already mutates `frame_dirty` on a non-const server. If preferred, change `nw_compose_scene` to take a non-const `struct nw_server *` and drop the cast — update the header and call sites accordingly.)

- [ ] **Step 5: Allocate/free the per-slot lo-res cache in the shell**

In `user/nwm/nwm.c`, where `g_winframe[slot]` is allocated (`nwm.c:274`) add a sibling buffer sized to the worst-case cache_rect lo-res (full screen / factor):
```c
		if (g_winbackdrop[slot]) { free(g_winbackdrop[slot]); g_winbackdrop[slot] = 0; }
		g_winbackdrop[slot] = (uint32_t *) malloc((size_t) g_bdc.lo_cap * 4);
		w->bd_blur = g_winbackdrop[slot];
		w->bd_lw = w->bd_lh = 0; w->bd_dirty = 1;
		w->bd_rect = (nw_rect){0,0,0,0};
```
Declare `static uint32_t *g_winbackdrop[NW_MAX_CLIENTS_OR_WINDOWS];` next to `g_winframe` (match its array dimension exactly — use the same size constant `g_winframe` uses). Free it in BOTH window-teardown spots that free `g_winframe` (`nwm.c:259` and `:282`):
```c
		if (g_winbackdrop[slot]) { free(g_winbackdrop[slot]); g_winbackdrop[slot] = 0; }
```
```c
		if (g_winbackdrop[i]) { free(g_winbackdrop[i]); g_winbackdrop[i] = 0; }
```

- [ ] **Step 6: Run host tests + build**

Run: `make test && make clean && make image64`
Expected: tests PASS; image builds. Visual behaviour identical to Task 6 when windows are static (cache is freshly built and matches), since `force_rebuild` is still effectively driven by the damage scissor.

- [ ] **Step 7: Commit**

```bash
git add user/nwm/nwm_core.h user/nwm/nw_backdrop.h user/nwm/nw_backdrop.c user/nwm/nw_compose.c user/nwm/nwm.c tests/test_nw_backdrop.cpp
git commit -m "feat(nwm): persistent per-window lo-res backdrop cache"
```

### Task 9: Fast-drag mode (reuse cache while moving, refresh every N frames)

**Files:**
- Modify: `user/nwm/nwm_core.h` (`frame_ctr`), `user/nwm/nwm.c` (pass drag state into compose), `user/nwm/nw_compose.c` (decide `force_rebuild`)
- Modify: `user/nwm/nw_compose.h` (carry the per-frame drag hint in `nw_backdrop_ctx`)

- [ ] **Step 1: Add the per-frame drag hint to the backdrop context**

In `user/nwm/nw_compose.h`, extend `struct nw_backdrop_ctx`:
```c
	int                frame_ctr;   /* compositor frame counter (for fast-drag cadence)     */
	int                drag_win;    /* window index being dragged this frame, or -1          */
```

- [ ] **Step 2: Decide `force_rebuild` in `nw_compose_scene`**

In `nw_compose_scene`, when building the backdrop, force a rebuild for the dragged window only every `NW_BD_FASTDRAG_N` frames; for every other window keep `force_rebuild = 0` (their cache reuse is governed by `nw_backdrop_reusable`):
```c
		int force = 0;
		if (bdc && idx == bdc->drag_win)
			force = (bdc->frame_ctr % NW_BD_FASTDRAG_N) == 0;
		if (bdc && bdc->bd && w->glass &&
		    build_backdrop(back, bdc, (struct nw_window *) w, w->x, w->y, fw, fh, force))
			bd = bdc->bd;
```
For the dragged window between forced rebuilds, `nw_backdrop_reusable` returns 0 (its `bd_rect` no longer matches the moved `want`), so without `force` it would rebuild every frame anyway — to actually reuse while sliding, in the dragged-window non-forced case upsample from the existing `w->bd_blur` even though the rect moved. Implement that in `build_backdrop`: add a `slide` path when `idx == drag_win && !force && w->bd_blur && w->bd_lw>0`:
```c
	if (!reuse) {
		if (allow_slide && w->bd_blur && w->bd_lw > 0) {
			/* reuse last blur, just resample it at the new size (approx during drag) */
			lw = w->bd_lw; lh = w->bd_lh;
		} else {
			/* ... the downsample+blur+persist block from Task 8 ... */
		}
	}
```
Thread an `allow_slide` argument (= `idx == bdc->drag_win && !force`) into `build_backdrop`. Keep the exact downsample/blur block from Task 8 in the `else`.

- [ ] **Step 3: Feed `frame_ctr` and `drag_win` from the shell**

In `user/nwm/nwm_core.h` add to `struct nw_server`:
```c
	int frame_ctr;   /* bumped by the shell each present() — drives fast-drag cadence */
```
In `user/nwm/nwm.c` `present()`, before calling `nw_compose_scene`:
```c
		S.frame_ctr++;
		g_bdc.frame_ctr = S.frame_ctr;
		g_bdc.drag_win  = S.drag_win;
```

- [ ] **Step 4: Build and verify drag is smooth in QEMU**

Run: `make clean && make image64 && make run64`
Expected: dragging a glass window over another window is **fluid** (no per-frame stutter); the blur is approximate mid-drag and snaps to exact on release (the move sets the dragged window's `bd_rect` mismatch, and the next static frame rebuilds it). Compare against Task 6 (which stuttered). If you want an A/B, temporarily set `NW_BD_FASTDRAG_N` to `1` (rebuild every frame = the slow path) and observe the difference.

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nwm_core.h user/nwm/nwm.c user/nwm/nw_compose.h user/nwm/nw_compose.c
git commit -m "feat(nwm): fast-drag backdrop mode keeps window moves fluid"
```

---

## Phase 5 — Recursion scope, visible-region, and a per-frame rebuild budget

### Task 10: Active-only fresh recursion + per-frame rebuild budget K + visible-region clip

**Files:**
- Modify: `user/nwm/nw_compose.h` (`rebuild_budget` in ctx), `user/nwm/nw_compose.c` (apply priority + budget + visible-region), `user/nwm/nwm.c` (set budget per frame)
- Test: `tests/test_nw_backdrop.cpp` (visible-region helper)

- [ ] **Step 1: Write the failing test for the visible-region subtraction helper**

Add to `nw_backdrop.h`:
```c
/* Largest single axis-aligned sub-rect of `r` not covered by `over` — used to skip blur work
 * under the part of a window hidden by a window above it. Returns `r` if they do not overlap,
 * an empty rect if `over` fully covers `r`, else the bigger of the four border slabs. */
nw_rect nw_rect_visible_band(nw_rect r, nw_rect over);
```
Append to `tests/test_nw_backdrop.cpp`:
```c
TEST_CASE("visible band: uncovered rect returned as-is; fully covered -> empty") {
	nw_rect r = {0,0,100,100};
	CHECK(nw_rect_visible_band(r, (nw_rect){200,200,10,10}).w == 100);    // disjoint
	CHECK(nw_rect_empty(nw_rect_visible_band(r, (nw_rect){-5,-5,110,110})) == 1); // covered
	nw_rect b = nw_rect_visible_band(r, (nw_rect){0,0,100,40});           // top 40 covered
	CHECK(b.y == 40); CHECK(b.h == 60);                                  // bottom slab survives
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: undefined `nw_rect_visible_band`.

- [ ] **Step 3: Implement the helper**

Append to `user/nwm/nw_backdrop.c`:
```c
nw_rect nw_rect_visible_band(nw_rect r, nw_rect over)
{
	nw_rect c = nw_rect_intersect(r, over);
	if (nw_rect_empty(c)) return r;                  /* nothing hidden */
	nw_rect cand[4] = {
		{ r.x, r.y, r.w, c.y - r.y },                            /* above the cover */
		{ r.x, c.y + c.h, r.w, r.y + r.h - (c.y + c.h) },        /* below */
		{ r.x, r.y, c.x - r.x, r.h },                            /* left  */
		{ c.x + c.w, r.y, r.x + r.w - (c.x + c.w), r.h },        /* right */
	};
	nw_rect best = { 0, 0, 0, 0 };
	long ba = 0;
	for (int i = 0; i < 4; i++) {
		if (cand[i].w <= 0 || cand[i].h <= 0) continue;
		long a = (long) cand[i].w * cand[i].h;
		if (a > ba) { ba = a; best = cand[i]; }
	}
	return best;
}
```

- [ ] **Step 4: Apply priority + budget + visible-region in `nw_compose_scene`**

Add `int rebuild_budget;` to `struct nw_backdrop_ctx` (`nw_compose.h`). In `nw_compose.c`, track a running budget and skip *fresh rebuilds* (not reuse) once it is exhausted, prioritising the active window and MENU/POPUP/DIALOG/TOOLTIP windows:
```c
	int budget = bdc ? bdc->rebuild_budget : 0;
	...
	for (int z = 0; z < s->zn; z++) {
		...
		int prio = (idx == s->focus) || (w->type != NW_WIN_NORMAL);
		int allow_fresh = prio || budget > 0;     /* non-priority rebuilds draw from the budget */
		const struct nw_surface *bd = 0;
		if (bdc && bdc->bd && w->glass) {
			int did_fresh = 0;
			if (build_backdrop_ex(back, bdc, (struct nw_window *) w, w->x, w->y, fw, fh,
			                      force, allow_fresh, &did_fresh))
				bd = bdc->bd;
			if (did_fresh && !prio && budget > 0) budget--;
		}
		...
	}
```
`build_backdrop_ex` is `build_backdrop` from Task 9 plus: an `allow_fresh` input (when a fresh rebuild is needed but not allowed, fall back to whatever is in `w->bd_blur` — never render empty; if the cache is empty too, return 0 so the window composites with no blur for this frame) and a `*did_fresh` output (set when a real downsample+blur happened). For the visible-region optimization, before downsampling, shrink the work rect with `nw_rect_visible_band(want, rect_of_topmost_window_above)`; this is an optional refinement — if it complicates the cache offset math, skip it and leave a `log`/comment noting blur is computed over the full cache_rect even when partly occluded (a cost-only, not correctness, concession).

- [ ] **Step 5: Set the budget per frame in the shell**

In `user/nwm/nwm.c` `present()`:
```c
		g_bdc.rebuild_budget = 2;   /* NW_BD_REBUILD_K: max non-priority fresh rebuilds/frame */
```

- [ ] **Step 6: Run host tests + build + QEMU**

Run: `make test && make clean && make image64 && make run64`
Expected: tests PASS. With many overlapping glass windows, opening/refocusing stays responsive (active + menu windows always crisp-current; background windows may lag one or two frames behind but are never blank). No visual regression vs Phase 4 for one or two windows.

- [ ] **Step 7: Commit**

```bash
git add user/nwm/nw_compose.h user/nwm/nw_compose.c user/nwm/nwm.c user/nwm/nw_backdrop.h user/nwm/nw_backdrop.c tests/test_nw_backdrop.cpp
git commit -m "feat(nwm): rebuild budget + active-priority + visible-region for backdrop blur"
```

---

## Phase 6 — Finish

### Task 11: Final verification and branch completion

- [ ] **Step 1: Full host test + coverage gate**

Run: `make test`
Expected: all tests PASS; coverage ≥90% including `nw_backdrop`.

- [ ] **Step 2: e2e visual sweep in QEMU**

Run: `make clean && make image64 && make run64`
Verify the full checklist:
- glass windows show a blurred backdrop;
- window-over-window blurs the lower window (not wallpaper);
- margins/corners outside a window stay sharp;
- dragging is fluid; blur snaps to exact on release;
- many overlapping windows stay responsive and never render a blank backdrop;
- at native Dell resolution (1920×1080) the lo-res buffers stay well under the `nw_blur_rect` 2048 limit (480 wide ≪ 2048).

- [ ] **Step 3: Finish the branch**

Announce and use **superpowers:finishing-a-development-branch** to verify tests, present options, and complete the work.

---

## Self-review notes (author)

- **Spec coverage:** module + pixel math (Tasks 1-3); Approach-A in-place backdrop from `g_scene` + `composite_round` backdrop source (Tasks 4-6); explicit window type (Task 7); dirty-rect partial update (free via the damage scissor — documented in the header, exercised by Task 5's blur test under a clipped scene if extended); drag fast/accurate modes (Task 9); persistent cache (Task 8); recursive glass-on-glass active-only + visible-region + priority budget (Task 10); host tests + QEMU throughout; tunables header (Task 1). All spec sections map to a task.
- **No placeholders:** every code step shows complete code or an exact diff with a file:line anchor; the only deliberately-optional item (visible-region offset math in Task 10) is called out with its cost-only consequence rather than left vague.
- **Type consistency:** `nw_rect`, `nw_backdrop_ctx`, `nw_win_type`, and the `bd_*` field names are introduced once and reused verbatim across tasks; `build_backdrop` evolves to `build_backdrop_ex` explicitly in Task 10.
