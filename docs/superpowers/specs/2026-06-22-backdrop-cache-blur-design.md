# Backdrop Cache Blur for Glass Windows — Design

**Date:** 2026-06-22
**Component:** NanWM compositor (`user/nwm/`, `user/libnw/`)
**Status:** Design approved; ready for implementation plan.

## Goal

Bring the real NanWM compositor up to the "glass" look of the `.claude/nanoos-ui/`
mockup (`backdrop-filter: blur`): each translucent window shows a **blurred** image of
whatever sits **below it in Z-order** — wallpaper, panels, and crucially **other windows**,
so a glass window over another window blurs that lower window, not the wallpaper. Correct,
overlap-aware, and affordable on a CPU-only framebuffer with no GPU and a single-threaded
compositor.

## Why not a single global blur layer

A single full-screen blurred copy of the desktop, sampled by all windows, is cheap but
**wrong**: a glass window stacked over another window would show blurred *wallpaper* through
the gap, never the window beneath. The whole point is per-window backdrop from the layers
actually below that window. Rejected.

## Current state (baseline)

- `user/nwm/nw_compose.c` — `nw_compose_scene()` paints wallpaper, then windows
  **back-to-front** via `composite_round()` (alpha blend + anti-aliased rounded corners,
  `NW_RADIUS 11`), then panel/taskbar/menu/run-dialog. Windows are already translucent
  glass (`WIN_ALPHA 234` light / `DARK_ALPHA 236` dark) — but composited over the **raw**
  scene, no blur.
- `user/libnw/nw_gfx.c:266` — `nw_blur_rect()` exists but is **never called**.
- Per-window **frame cache** (`nwm_core.h:69`): `frame` (chrome+content rendered once),
  `frame_dirty`; window **move (x/y) does NOT set frame_dirty** → drags are cheap.
- **Dirty-region present** (`nwm.c:363` `present()`): aggregates scene damage
  (`dmg_x0/y0/x1/y1`), scissors `nw_compose_scene` + cursor blit to the damage bbox.
- Pixel format: 32bpp `0x00RRGGBB`; blend via `nw_blend8()` (RB-paired); bilinear upscale
  already used for the wallpaper.
- No window `type` field; light/dark distinguished by title's first byte (`\x01`).

The back-to-front composite + per-window frame cache + dirty-region present are the ideal
foundation: when we are about to paint window W, `g_scene` already holds *exactly* the
layers below W — that **is** W's backdrop snapshot, for free, including already-composited
(already-blurred) lower glass windows. Window-on-window blur therefore falls out naturally.

## Architecture (Approach A: backdrop computed in-place from `g_scene` mid-composite)

Decisions locked in brainstorming:
- **Scope:** the full design (cache + dirty-rect partial update + drag modes + recursive
  glass-on-glass + visible_region clipping + update priorities).
- **Window type:** an explicit `type` field on `nw_window` (not a title-byte heuristic).
- **Verification:** host tests for the pure pixel/rect math, plus visual QEMU verification.

### New module `user/nwm/nw_backdrop.{c,h}` (pure, host-testable, no IO)

Kept free of `/dev/fb0` and libnw IO so it compiles and runs on the host test harness.

```c
typedef struct { int x, y, w, h; } nw_rect;   /* screen coords; w/h >= 0; empty if w==0||h==0 */

/* Rect helpers */
nw_rect nw_rect_expand(nw_rect r, int by);                 /* grow by `by` on every side */
nw_rect nw_rect_clamp(nw_rect r, int sw, int sh);          /* clamp to [0,sw)x[0,sh) */
nw_rect nw_rect_intersect(nw_rect a, nw_rect b);           /* empty if disjoint */
nw_rect nw_rect_union(nw_rect a, nw_rect b);
int     nw_rect_empty(nw_rect r);
int     nw_rect_intersects(nw_rect a, nw_rect b);
nw_rect nw_cache_rect(nw_rect win, int blur_radius, int sw, int sh); /* win expanded by radius, clamped */

/* Pixel math (operate on 0x00RRGGBB buffers) */
void nw_downsample_box(const uint32_t *src, int sw, int sh, int src_stride,
                       uint32_t *dst, int factor);          /* dst is sw/factor x sh/factor */
void nw_box_blur_h(uint32_t *buf, int w, int h, int radius);
void nw_box_blur_v(uint32_t *buf, int w, int h, int radius);
void nw_blur_passes(uint32_t *buf, int w, int h, int radius, int passes); /* H,V repeated */
void nw_upsample_bilinear(const uint32_t *lo, int lw, int lh,
                          uint32_t *out, int ow, int oh, int out_stride);
```

- `nw_downsample_box`: averaging box downsample, default `factor = 4` (16× fewer pixels to
  blur). Channels averaged with RB-paired arithmetic where convenient; correctness over
  micro-opt in the pure module.
- `nw_box_blur_h/v`: separable sliding-window box blur, O(pixels) per pass independent of
  radius. `nw_blur_passes(...,passes=3)` ≈ Gaussian.
- `nw_upsample_bilinear`: reuses the wallpaper bilinear approach (16.16 fixed point).
- Blur radius applied at lo-res = `BLUR_RADIUS / factor`.

### `struct nw_window` additions (`user/nwm/nwm_core.h`)

```c
enum nw_win_type { NW_WIN_NORMAL, NW_WIN_MENU, NW_WIN_POPUP, NW_WIN_DIALOG, NW_WIN_TOOLTIP };

uint8_t  type;        /* default NW_WIN_NORMAL */
uint8_t  glass;       /* 1 => gets backdrop blur (all windows for now) */

uint32_t *bd_blur;    /* per-window LO-RES blurred backdrop cache (cache_rect/factor sized) */
int       bd_lw, bd_lh;
nw_rect   bd_rect;    /* the cache_rect (screen coords) bd_blur was computed for */
int       bd_dirty;   /* 1 => backdrop must be (re)built; separate from frame_dirty */
```

- Only the **lo-res** blurred buffer is stored per window (~16× smaller). The full-res
  snapshot and full-res blur scratch are **shared scratch buffers** (one screen-cache_rect
  worth), not per window — bounds peak RAM.
- `bd_blur` is lazily allocated on first blur, freed on window close (next to `frame`).

### Window type propagation (`user/libnw/`)

`type` is threaded through the window-creation path in `libnw` (new attribute/arg, default
`NW_WIN_NORMAL`). The compositor sets `NW_WIN_MENU`/`NW_WIN_DIALOG` for its own app-menu and
run-dialog. `glass` defaults to 1 for every window (matching today's universal translucency).

## Per-frame flow (`nw_compose_scene`)

```
nw_compose_scene(scene, damage):
  paint_wallpaper(scene)                       # gradient / decoded wallpaper
  for w in zorder back-to-front:
      if w.glass:
          rect = nw_cache_rect(w.rect, BLUR_RADIUS, SW, SH)
          if backdrop_needs_rebuild(w, rect, damage):       # see Invalidation
              region = rebuild_region(w, rect, damage)       # full rect, or dirty sub-rect
              snapshot = scratch_copy(scene, region)         # g_scene already has all below w
              lo = nw_downsample_box(snapshot, factor=4)
              nw_blur_passes(lo, BLUR_RADIUS/4, passes=3)
              store_lowres(w.bd_blur at region-offset, lo)   # full rect or patched sub-rect
              w.bd_rect = rect; w.bd_dirty = 0
          bg = nw_upsample_bilinear(w.bd_blur -> rect.size)  # into shared full-res scratch
          composite_round(scene, w, backdrop=bg)             # content over blurred bg
      else:
          composite_round(scene, w, backdrop=NULL)           # flat tint (today's path)
  paint_panel_menu_dialog(scene)
```

`composite_round()` gains an optional `backdrop` (full-res buffer of the blurred region
under the window). When present: window body = `blend(backdrop, glass_tint, alpha)`, then
chrome, then rounded-rect mask + border + highlight, then **content (text/icons) drawn last
and sharp** — content never enters the blur. When `backdrop == NULL`, behaviour is exactly
today's flat-tint path.

Because `composite_round` reads `backdrop` from `g_scene` *before* it paints W, and lower
glass windows were already composited (already blurred) into `g_scene`, **window-on-window
blur is automatic** and fully correct.

**Steady state (window still, nothing below changed):** `backdrop_needs_rebuild == false`
→ skip snapshot+blur entirely; only the cheap bilinear upscale + composite run, and
`present()` still scissors to the damage bbox.

## Invalidation

A window's backdrop is stale when anything **below** it, or the window **itself**, changes.

| Event | Marks `bd_dirty` on |
|---|---|
| lower window redrew content (`frame_dirty`) | every glass window whose `bd_rect` intersects its rect |
| lower window moved | same, for **old and new** rects |
| Z-order change | glass windows from the lowest affected upward |
| glass window moved | that window (its underlying backdrop changed) |
| resize / radius / wallpaper change | that window (resize ⇒ reallocate lo-res) |

**Driver:** after the compositor computes scene damage (`dmg_*`), one pass over `zorder`:
for each glass window, if `nw_rect_intersects(bd_rect, damage)` (or the window itself is in
damage), set `bd_dirty = 1`. Cheap, single pass.

**Dirty-rect partial update (no shortcut):** when only a *part* of `bd_rect` is dirty, do
not rebuild the whole backdrop. Compute `dirty_blur_region = intersect(damage, bd_rect)`
**expanded by `BLUR_RADIUS`** (then clamped to `bd_rect`), downsample+blur only that
sub-rect, and patch it into the lo-res cache at the correct offset. Expanding by the radius
prevents seams at the patch boundary. Fall back to a **full rebuild** when `bd_dirty` came
from a resize, or when the dirty area covers ≥ ~70% of `bd_rect` (a configurable threshold;
below it partial wins, above it partial is not worth the bookkeeping).

## Drag modes

`S.dragging` / `S.drag_win` already exist.

- **Fast (during drag):** do **not** rebuild the dragged glass window's backdrop every
  frame. Reuse the last lo-res `bd_blur`, sliding the sample origin with `bd_rect` (the blur
  "moves" under the window — an approximation, but smooth). Every N frames
  (`S.frame_ctr % N == 0`, default N=4) do one fresh rebuild so the blur cannot drift too
  far from truth. Keeps motion fluid on CPU-only.
- **Accurate (on release):** `mouseup` sets `bd_dirty = 1` on the dragged window and on any
  windows it newly exposed/occluded → the next frame computes the exact backdrop for the
  final position.

Both behind a `NW_BACKDROP_FASTDRAG` compile flag for A/B comparison in QEMU.

## Recursive glass-on-glass, visible_region, priorities

- **Glass-on-glass:** the fully-correct mode is free under Approach A (lower windows are
  already in `g_scene`). To bound cost, only **active elements** force a fresh snapshot that
  accounts for other glass windows every frame: the active window and any
  `NW_WIN_MENU/POPUP/DIALOG/TOOLTIP`. Other glass windows, when not active and with nothing
  changed below them, ride their existing cache (which was itself computed from `g_scene`,
  so it is correct, just not refreshed every frame — not a "simplified" backdrop).
- **visible_region:** when a glass window is partly covered by a window **above** it, compute
  `visible = bd_rect − ∪(rects of windows above)` and limit blur/composite to the visible
  sub-rects (reuse `nw_rect_*`). Fewer pixels under stacks of windows.
- **Update priorities:** if several windows are `bd_dirty` in one frame, order rebuilds by
  (1) active window, (2) window under the cursor, (3) menu/popup/dialog/tooltip,
  (4) visible glass windows, (5) partially-covered glass windows. Budget: at most `K` full
  rebuilds per frame (default K=2); the rest stay `bd_dirty` for the next frame (showing a
  slightly stale but **present** blur — never empty). Invisible windows: skipped.

## Tunables (single header, easy A/B in QEMU)

```c
#define NW_BD_DOWNSAMPLE   4     /* downsample factor */
#define NW_BD_BLUR_RADIUS  24    /* full-res blur radius (mockup uses 24-26px) */
#define NW_BD_BLUR_PASSES  3     /* box passes ~= Gaussian */
#define NW_BD_FASTDRAG_N   4     /* fresh rebuild every N frames while dragging */
#define NW_BD_REBUILD_K    2     /* max full rebuilds per frame */
#define NW_BD_PARTIAL_MAX  70    /* % of cache_rect above which partial update is skipped */
#define NW_BACKDROP_FASTDRAG 1   /* enable fast-drag mode */
```

## Files

**New:**
- `user/nwm/nw_backdrop.h`, `user/nwm/nw_backdrop.c` — pure rect + pixel math.
- `tests/test_nw_backdrop.cpp` — host tests (doctest).

**Modified:**
- `user/nwm/nwm_core.h` — `nw_win_type`, `type`/`glass`/`bd_*` fields on `nw_window`.
- `user/nwm/nw_compose.c` / `nw_compose.h` — backdrop step in `nw_compose_scene`;
  `composite_round` gains optional `backdrop`.
- `user/nwm/nwm_core.c` — invalidation pass; priority/budget bookkeeping; drag-mode hooks.
- `user/nwm/nwm.c` — window-close frees `bd_blur`; shared scratch alloc; frame counter.
- `user/libnw/` (window-creation path) — thread `type` through; default `NW_WIN_NORMAL`.
- `Makefile` — add `nw_backdrop` to `TEST_MODULES` / `COV_PATTERNS` (≥90% gate).

## Verification

**Host tests (`tests/`, doctest — like the PNG decoder):** `nw_backdrop.c` is pure, so it
compiles for the host harness.
- `nw_box_blur_h/v`: blurring a constant color returns the same color (DC preserved); a
  single bright pixel blurs **symmetrically**; total "energy" preserved within rounding.
- `nw_downsample_box`: a 4×4 block of one color → one pixel of that color; a gradient → the
  correct average.
- `nw_upsample_bilinear`: known lo-res → known midpoint values (reuse wallpaper tests).
- `nw_cache_rect` / `nw_rect_*`: expand-by-radius, clamp-to-screen, intersect/union over a
  table of edge cases (window at screen edge, fully off-screen, just touching).
- invalidation decision: given a damage rect and a set of windows, assert which get
  `bd_dirty` and the computed `dirty_blur_region` (expanded by radius). Pure decision
  function, testable without drawing.
- Coverage gate ≥90% on the new module (`COV_PATTERNS`).

**QEMU (headless, visual):** build `image64` → `screendump` → PNG; confirm: blur visible
under a window; window-over-window shows the **blurred lower window** (not wallpaper); drag
stays fluid; no seams from dirty-rect partial updates; no corner artifacts. A/B via
`NW_BACKDROP_*` flags.

## Invariants & risks

- This is **userland** (not MI) — `make check-arch` is N/A; keep `nw_backdrop` free of IO so
  host tests remain possible.
- Largest risk is **CPU cost** — mitigated by lo-res cache, the K-rebuilds budget, and
  fast-drag. Second risk is **seams in dirty-rect partial updates** — mitigated by expanding
  the dirty region by the blur radius and by symmetry host tests.
- A backdrop cache must **never render empty**: when a rebuild is budget-deferred, the window
  shows its last (slightly stale) blur, not a blank.
