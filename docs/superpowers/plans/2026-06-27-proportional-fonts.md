# Proportional TTF Font System Implementation Plan

> **For agentic workers:** use superpowers:executing-plans. Steps use `- [ ]`.

**Goal:** A runtime TTF/OTF font engine (stb_truetype) rendering anti-aliased **proportional** UI
text across the toolkit, with the terminal on a **monospace** role, and font selection in Settings.

**Spec:** `docs/superpowers/specs/2026-06-27-proportional-fonts-design.md`. **Branch:** `feat/rust-explorer`.

**Conventions:** no Claude attribution in commits; never `git add CLAUDE.md`; `make check-arch` after MI
edits; host tests `make test64`; image `make image64`; QEMU per CLAUDE.md headless pattern. Keep the
1-bit `nx_font8x16` fallback at every step so text never goes blank.

---

## Phase 1 — Font engine foundation (stb_truetype + nwfont), 1-bit fallback intact

### Task 1.1: Vendor + compile stb_truetype in libnw
- Files: `user/third_party/stb/stb_truetype.h` (done), create `user/libnw/stb_impl.c`, modify Makefile.
- [ ] `stb_impl.c`: `#define STB_TRUETYPE_IMPLEMENTATION`, `#define STBTT_assert(x)`, `STBTT_malloc(x,u)=malloc(x)`, `STBTT_free(x,u)=free(x)`, then `#include "stb_truetype.h"` (math via picolibc). Add `-Iuser/third_party/stb` to USER_CFLAGS.
- [ ] Add `stb_impl.o` to the libnw object set (where `nw_gfx.o` is built/linked: grep `nw_gfx.o` in Makefile; libnw.ndl objects + nwm.nxe).
- [ ] Build `make image64` far enough to confirm stb_impl compiles + links (no missing libm). Expected: clean.
- [ ] Commit: `feat(libnw): vendor stb_truetype + freestanding impl unit`.

### Task 1.2: nwfont module (load, scale, glyph cache, metrics)
- Files: create `user/libnw/nwfont.{c,h}`; modify Makefile (add nwfont.o next to nw_gfx.o).
- [ ] `nwfont.h`: roles `NWFONT_UI/NWFONT_MONO`; `struct nwfont_glyph { const unsigned char *cov; int w,h,advance,bx,by; }`; API from spec §A.
- [ ] `nwfont.c`: per role keep `{ unsigned char *file; stbtt_fontinfo info; float scale; int ascent, line_h; struct nwfont_glyph cache[256]; unsigned char *bitmaps[256]; int loaded; }`. `nwfont_set(role,path,px)`: read file (cap 4 MiB), `stbtt_InitFont`, `scale=stbtt_ScaleForPixelHeight`, fill ascent/line_h, clear cache. `nwfont_get(role,cp)`: lazily `stbtt_GetCodepointBitmap` + `GetCodepointHMetrics`/`GetCodepointBitmapBox`, store. `nwfont_text_w`: sum advances.
- [ ] Host test `tests/test_nwfont.cpp`: load `assets/fonts/UISans-Regular.ttf`, assert `text_w("")==0`, `text_w("WW")>text_w("W")>0`, glyph('A') has w/h>0 and advance>0. Add `user/libnw/nwfont.c` + `stb_impl.c` to TEST_MODULES (not gated — stb is large/3rd-party).
- [ ] `make test64` green. Commit: `feat(libnw): nwfont TTF engine (load + AA glyph cache + metrics)`.

## Phase 2 — nw_gfx proportional UI text + mono terminal path

### Task 2.1: Wire nwfont into nw_gfx
- Files: `user/libnw/nw_gfx.{c,h}`, Makefile (font install), `arch/...` none.
- [ ] Add `nw_text_w(const char*)` (UI width) + `nw_font_init()` (load UI+MONO from `/nanos/share/fonts`, UI name param) to nw_gfx.h.
- [ ] `nw_text`: if UI font loaded, render proportional AA (blend coverage via `nw_blend_pixel`, advance per glyph, baseline = y + ascent); else 1-bit fallback. Return end x.
- [ ] `nw_draw_char` + opaque path: render MONO glyph AA into the fixed cell (blend over bg); fallback 1-bit. Keep `NW_FONT_W` as MONO advance; set `NW_FONT_H` = UI line box (18) — verify terminal uses its own cell metric (mono px), not NW_FONT_H, or parametrize.
- [ ] `_image64`: `mkdir /nanos/share/fonts`; install `assets/fonts/UISans-Regular.ttf` + `Mono-Regular.ttf`.
- [ ] nwm + apps call `nw_font_init()` at startup (one call in the gfx/app init path).
- [ ] Build `make image64`; QEMU screendump: UI text smooth/proportional, terminal still aligned. Commit.

## Phase 3 — libnwui proportional layout

### Task 3.1: Measure/paint widths
- [ ] `nwui_measure`: LABEL/BUTTON/CHECKBOX/LINK mw via `nw_text_w(n->text)` (+ pad). Heights from `NW_FONT_H`.
- [ ] `nwui_paint`: center button/link/iconview label via `nw_text_w`. Iconview label truncation by pixel width.
- [ ] Host test: a label's measured mw equals `nw_text_w` of its text. `make test64`. Commit.

### Task 3.2: Textfield + textarea pixel math (the intricate part)
- [ ] `char_at_x` + caret x: walk buffer glyph advances (UI font) to map pixel↔index.
- [ ] Textarea wrapping: replace `cols = w/NW_FONT_W` with pixel-accumulated line breaks; caret/selection/hit by advances.
- [ ] Host tests for wrap breaks + caret mapping on a known string/width. `make test64` green. Commit.

## Phase 4 — Settings font management + hot reload

### Task 4.1: settings model
- [ ] `nw_settings`: add `ui_font[64]` + parse/serialize + getter; default `UISans-Regular.ttf`. Host test parse round-trip. Commit.

### Task 4.2: nwset UI + reload
- [ ] nwset: "Fonts" group lists `/nanos/share/fonts/*.{ttf,otf}` (nwui_dir_*) as rows; pick → write setting → reload broadcast.
- [ ] Reload path: on settings reload, call `nwfont_set(NWFONT_UI, path, px)` + repaint (compositor + apps via existing reload signal). Build + QEMU: switch font live. Commit.

## Phase 5 — rsexp "Install font" (optional polish)
- [ ] In rsexp, double-clicking a `.ttf`/`.otf` copies it into `/nanos/share/fonts/` (a reusable `nwui` copy helper or libc read/write) and reports. Build + QEMU. Commit.

## Phase 6 — Verify + finish
- [ ] `make check-arch` clean; `make test64` green; QEMU: smooth proportional UI, mono terminal, Settings font switch live, no faults. Screendumps.
- [ ] superpowers:finishing-a-development-branch.

## Risks
- stb_truetype freestanding link (libm) — de-risk in Task 1.1.
- Textarea pixel-wrap (Task 3.2) — host-test first.
- Terminal monospace alignment — verify in QEMU after Phase 2.
- Keep 1-bit fallback throughout.
