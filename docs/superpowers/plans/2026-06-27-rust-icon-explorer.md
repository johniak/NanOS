# Rust Icon File Explorer (XP-style, "My Computer") Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a Windows-XP-style file explorer written from scratch in Rust on the x86_64 NanOS desktop (nwm), with a big-icon grid, PNG icons from disk, an XP left task pane, and a virtual "My Computer" root listing each `/disks/*` volume plus Home — reusing every general-purpose piece via the C `libnwui` toolkit.

**Architecture:** All reusable widgets/assets land in the C toolkit `libnwui` (a PNG image loader, an `NWUI_ICONVIEW` grid widget, an `NWUI_PANEL` titled group-box, and an icon set). A new x86_64 Rust target + a safe `libnwui-rs` binding crate let the explorer app (`rsexp`) be thin idiomatic Rust over that C ABI, exactly like the existing i686 `rustform` demo. "My Computer" is userland-synthetic (enumerate `/disks` + `$HOME`); no kernel change.

**Tech Stack:** C (freestanding userland, libnwui toolkit, doctest host tests), Rust (`no_std`, `-Z build-std`, cargo staticlib linked through `mknx`), Python stdlib (host-side icon PNG generator), Makefile + `debugfs` image install, QEMU headless screendump verification.

**Spec:** `docs/superpowers/specs/2026-06-27-rust-icon-explorer-design.md`

**Branch:** `feat/rust-explorer` (already created off `main`; the spec is already committed there).

---

## Conventions for every task

- **No Claude/AI attribution** in any commit message (project rule). Clean messages only.
- **Never `git add CLAUDE.md`.**
- Toolkit code (`libnwui`, the PNG module, the icon generator) is **machine-independent** — after touching it run `make check-arch` and expect it to stay clean.
- Host tests are doctest C++ in `tests/`. The kernel/MI host-test suites are `make test` and `make test64`; run whichever the test list change targets (the libnwui modules are MI, so they compile into the host suite). To find the exact module/coverage lists, grep the Makefile: `grep -n "test_nwui\|TEST_MODULES\|HOST_TEST\|test64" Makefile`.
- The GUI ships **x86_64 only** (`make image64`). The icon-grid and panel widgets must also compile into the host test binary (they are pure C, no gfx in the core path).
- QEMU verification uses the established headless pattern (see CLAUDE.md "Verifying a kernel change in QEMU"): boot `make image64` with `-display none` + monitor socket, `screendump` to PPM, `sips -s format png`, read the PNG.

---

## File Structure (what each new/changed file is responsible for)

**Created**
- `user/libnwui/nwui_png.c` / `nwui_png.h` — the PNG decoder (moved from nwm) + a `nwui_image_load_png(path,…)` file wrapper. One decoder for the whole system.
- `assets/icons/*.png` — committed 48×48 RGBA icon set (`computer drive folder home program text image file`).
- `tools/gen-icons.py` — host-only Python (stdlib) generator that produces `assets/icons/*.png`.
- `user/rust/x86_64-nanos.json` — the 64-bit Rust target spec.
- `user/rust/libnwui-rs/` — reusable safe Rust bindings crate (`Cargo.toml`, `src/lib.rs`).
- `user/rust/rsexp/` — the explorer app crate (`Cargo.toml`, `src/lib.rs`, `src/nanos.rs` runtime shim).
- `tests/test_nwui_iconview.cpp`, `tests/test_nwui_panel.cpp`, `tests/test_nwui_png.cpp` — toolkit host tests.

**Modified**
- `user/libnwui/nwui.h` — public API: `nwui_image_load_png`, `nwui_iconview*`, `nwui_panel`, `nwui_icon_item`.
- `user/libnwui/nwui_core.h` — node enum + `nwui_node` fields for iconview/panel + cell constants.
- `user/libnwui/nwui_core.c` — constructors + measure/arrange + event handling for iconview/panel.
- `user/libnwui/nwui_paint.c` — paint cases for iconview + panel.
- `user/nwm/png.c` / `png.h` — become thin forwarders to `nwui_png` (dedup).
- `user/nwm/nwm_core.c` — add "My Computer" to `LOGO_ITEMS`, bump `LOGO_NITEMS`, spawn handler.
- `Makefile` — x64 Rust app rule; add `rsexp` to `X64_GUI_APPS`, drop `nwexp`; install `/nanos/share/icons/`; add the three host tests to the toolkit test list.

---

## Phase 1 — PNG image loader in libnwui (reuse: one decoder system-wide)

### Task 1.1: Move the PNG decoder into libnwui

**Files:**
- Create: `user/libnwui/nwui_png.h`, `user/libnwui/nwui_png.c`
- Modify: `user/nwm/png.h`, `user/nwm/png.c`

- [ ] **Step 1: Copy the decoder into the toolkit**

```bash
git mv user/nwm/png.c user/libnwui/nwui_png.c
git mv user/nwm/png.h user/libnwui/nwui_png.h
```

- [ ] **Step 2: Rename the include guard + add the file wrapper declaration**

Edit `user/libnwui/nwui_png.h` so it reads:

```c
/*
 * nwui_png.h — a tiny self-contained PNG decoder for the toolkit (no libpng/zlib).
 * Decodes a non-interlaced 8-bit PNG (greyscale / RGB / RGBA / palette) to a flat
 * 32bpp 0x00RRGGBB buffer. Returns a malloc'd buffer (caller frees), or NULL on error.
 */
#ifndef NWUI_PNG_H
#define NWUI_PNG_H

#include <stdint.h>

/* Decode `len` bytes of PNG at `data`: malloc'd w*h 0x00RRGGBB array + dimensions, 0 on failure. */
uint32_t *png_decode(const uint8_t *data, unsigned len, int *w, int *h);

/* Read a PNG file from disk and decode it: malloc'd w*h 0x00RRGGBB array + dimensions, 0 on any
 * failure (missing file, unsupported/corrupt PNG, OOM). Caller frees. */
uint32_t *nwui_image_load_png(const char *path, int *w, int *h);

#endif /* NWUI_PNG_H */
```

- [ ] **Step 3: Update `nwui_png.c`'s own include + add the file wrapper**

At the top of `user/libnwui/nwui_png.c`, change `#include "png.h"` → `#include "nwui_png.h"` and ensure libc headers for file I/O are included (`#include <fcntl.h>`, `#include <unistd.h>`, `#include <stdlib.h>`). Append the wrapper at the end of the file:

```c
/* Read an entire file into a malloc'd buffer, then png_decode it. */
uint32_t *nwui_image_load_png(const char *path, int *w, int *h)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    /* Icons are tiny; cap the read at 256 KiB like nwm's wallpaper loader. */
    unsigned cap = 256u * 1024u;
    uint8_t *file = (uint8_t *) malloc(cap);
    if (!file) { close(fd); return 0; }
    int total = 0, got;
    while (total < (int) cap && (got = read(fd, file + total, cap - total)) > 0)
        total += got;
    close(fd);
    uint32_t *px = (total > 0) ? png_decode(file, (unsigned) total, w, h) : 0;
    free(file);
    return px;
}
```

- [ ] **Step 4: Expose the loader from the public header**

In `user/libnwui/nwui.h`, add near the image widget declaration:

```c
/* Load a PNG file into a malloc'd w*h 0x00RRGGBB buffer (caller frees). 0 on any failure. */
uint32_t *nwui_image_load_png(const char *path, int *w, int *h);
```

- [ ] **Step 5: Point nwm at the toolkit decoder (dedup)**

`user/nwm/png.c` and `png.h` no longer exist. In `user/nwm/nwm.c`, change `#include "png.h"` → `#include "nwui_png.h"` (the build already has `-Iuser/libnwui` on GUI app include paths; if not, the Makefile nwm rule must add it — verify with `grep -n "nwm.o" Makefile` and ensure `-Iuser/libnwui` is present in the GUI CFLAGS). `png_decode`'s signature is unchanged, so the call site `png_decode(file, got, &iw, &ih)` is untouched.

- [ ] **Step 6: Build the image and confirm the wallpaper still loads**

Run: `make image64`
Expected: clean build (the linker pulls `nwui_png.o` into nwm via libnwui). No undefined `png_decode`.

- [ ] **Step 7: Commit**

```bash
git add user/libnwui/nwui_png.c user/libnwui/nwui_png.h user/nwm/nwm.c user/libnwui/nwui.h Makefile
git commit -m "feat(libnwui): promote the PNG decoder into the toolkit + nwui_image_load_png"
```

### Task 1.2: Host test for the PNG file loader (decode round-trip)

**Files:**
- Create: `tests/fixtures/icon_test.png` (a tiny known PNG), `tests/test_nwui_png.cpp`
- Modify: `Makefile` (host test list)

- [ ] **Step 1: Create a known 2×2 RGB PNG fixture with Python**

```bash
python3 - <<'PY'
import zlib, struct
def chunk(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
w,h=2,2
# rows: filter byte 0 then RGB triples. pixels: red, green / blue, white
raw=b"\x00\xff\x00\x00\x00\xff\x00"+b"\x00\x00\x00\xff\xff\xff\xff"
png=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",w,h,8,2,0,0,0))+chunk(b"IDAT",zlib.compress(raw))+chunk(b"IEND",b"")
open("tests/fixtures/icon_test.png","wb").write(png)
print("wrote", len(png), "bytes")
PY
```

- [ ] **Step 2: Write the failing test**

`tests/test_nwui_png.cpp`:

```cpp
#include "doctest.h"
#include "nwui_png.h"

TEST_CASE("nwui_image_load_png decodes a known 2x2 RGB PNG") {
    int w = 0, h = 0;
    uint32_t *px = nwui_image_load_png("tests/fixtures/icon_test.png", &w, &h);
    REQUIRE(px != nullptr);
    CHECK(w == 2);
    CHECK(h == 2);
    CHECK((px[0] & 0x00ffffff) == 0x00ff0000); // red
    CHECK((px[1] & 0x00ffffff) == 0x0000ff00); // green
    CHECK((px[2] & 0x00ffffff) == 0x000000ff); // blue
    CHECK((px[3] & 0x00ffffff) == 0x00ffffff); // white
    free(px);
}

TEST_CASE("nwui_image_load_png returns null for a missing file") {
    int w = 0, h = 0;
    CHECK(nwui_image_load_png("tests/fixtures/does_not_exist.png", &w, &h) == nullptr);
}
```

- [ ] **Step 3: Register the test + module in the Makefile**

Find the libnwui host-test list (`grep -n "test_nwui_core" Makefile`). Add `tests/test_nwui_png.cpp` to the test sources and `user/libnwui/nwui_png.c` to the compiled-under-test modules, mirroring how `test_nwui_core.cpp` + `nwui_core.c` are listed. The test runs from the repo root so the relative fixture path resolves.

- [ ] **Step 4: Run the test — expect PASS**

Run: `make test` (or `make test64` — whichever the toolkit tests are wired into; check the grep output)
Expected: the two new PNG cases pass; coverage gate still green.

- [ ] **Step 5: Commit**

```bash
git add tests/test_nwui_png.cpp tests/fixtures/icon_test.png Makefile
git commit -m "test(libnwui): PNG file loader decode round-trip + missing-file guard"
```

---

## Phase 2 — `NWUI_ICONVIEW` icon-grid widget (reuse: any app gets icon grids)

### Task 2.1: Declare the iconview type, item struct, and fields

**Files:**
- Modify: `user/libnwui/nwui_core.h`, `user/libnwui/nwui.h`

- [ ] **Step 1: Add the node kind + cell constants**

In `user/libnwui/nwui_core.h`, extend the kind enum and add cell constants:

```c
enum { NWUI_BOX, NWUI_ROW, NWUI_COLUMN, NWUI_LABEL, NWUI_BUTTON, NWUI_TEXTFIELD, NWUI_LIST, NWUI_IMAGE,
       NWUI_TEXTAREA, NWUI_CHECKBOX, NWUI_ICONVIEW, NWUI_PANEL };

enum { NWUI_ICON_CELL_W = 92, NWUI_ICON_CELL_H = 84 };  /* icon grid cell box */
enum { NWUI_ICON_PX = 48 };                              /* nominal icon size */
enum { NWUI_PANEL_TITLE_H = 22 };                        /* titled-panel header height */
```

- [ ] **Step 2: Add the iconview/panel fields to `nwui_node`**

In `struct nwui_node` (after the list fields at `nwui_core.h:53-56`), add:

```c
    /* iconview: app-owned array of cells (label + icon pixels). Reuses count/sel/scroll/last_* above. */
    const struct nwui_icon_item *icons;
    int        cols;                 /* iconview: columns computed at arrange time */
    /* panel: title is stored in `text`; children stack vertically under the header. */
```

- [ ] **Step 3: Declare the public item struct + API in `nwui.h`**

In `user/libnwui/nwui.h` (near the list API):

```c
/* One icon-grid cell: a label plus an icon (app-owned w*h 0x00RRGGBB buffer; may be shared). */
typedef struct { const char *label; const uint32_t *icon; int iw, ih; } nwui_icon_item;

/* An icon-grid view. on_change fires on single-click (selection change); on_activate fires on
 * double-click / Enter. Read the selection with nwui_iconview_selected(). */
nwui_node *nwui_iconview(nwui *u, nwui_cb on_activate, nwui_cb on_change, void *user);
void       nwui_iconview_set(nwui_node *n, const nwui_icon_item *items, int count);
int        nwui_iconview_selected(nwui_node *n);

/* A titled panel (glass group-box). Add children with nwui_add(); they stack vertically. */
nwui_node *nwui_panel(nwui *u, const char *title);
```

- [ ] **Step 4: Commit (declarations only — builds via the next task)**

Defer committing until Task 2.2 compiles; declarations alone won't link. Proceed directly.

### Task 2.2: Implement iconview core (constructor, set, measure, arrange, selection)

**Files:**
- Modify: `user/libnwui/nwui_core.c`
- Test: `tests/test_nwui_iconview.cpp`

- [ ] **Step 1: Write the failing test for layout + selection math**

`tests/test_nwui_iconview.cpp`:

```cpp
#include "doctest.h"
#include "nwui_core.h"
#include "nwui.h"
#include "libnw.h"
#include <cstring>

static int g_act, g_chg;
static void on_act(nwui_node *, void *u) { (*(int *) u)++; }
static void on_chg(nwui_node *, void *) { g_chg++; }

static void pointer(nwui *u, int x, int y, int b) {
    nw_event e; memset(&e, 0, sizeof e);
    e.type = NW_EV_POINTER; e.x = x; e.y = y; e.buttons = b;
    nwui_dispatch(u, &e);
}
static void click(nwui *u, int x, int y) { pointer(u, x, y, 1); pointer(u, x, y, 0); }

static uint32_t dummy[4] = {0,0,0,0};

TEST_CASE("iconview computes columns from width and selects the clicked cell") {
    nwui *u = new nwui; nwui_init(u);
    g_act = 0; g_chg = 0;
    static nwui_icon_item items[5];
    for (int i = 0; i < 5; i++) { items[i].label = "x"; items[i].icon = dummy; items[i].iw = 2; items[i].ih = 2; }
    nwui_node *iv = nwui_iconview(u, on_act, on_chg, &g_act);
    nwui_iconview_set(iv, items, 5);
    nwui_set_root(u, iv);
    u->win_w = 300; u->win_h = 300;     // 300 / 92 -> 3 columns
    nwui_layout(u);
    CHECK(iv->cols == 3);
    CHECK(nwui_iconview_selected(iv) == -1);

    // click the cell at row 0, col 1 -> index 1
    int cx = iv->x + NWUI_ICON_CELL_W + NWUI_ICON_CELL_W / 2;
    int cy = iv->y + NWUI_ICON_CELL_H / 2;
    click(u, cx, cy);
    CHECK(nwui_iconview_selected(iv) == 1);
    CHECK(g_chg == 1);          // single click => on_change
    CHECK(g_act == 0);          // not a double click
    delete u;
}

TEST_CASE("iconview double-click on the same cell activates") {
    nwui *u = new nwui; nwui_init(u);
    g_act = 0; g_chg = 0;
    static nwui_icon_item items[2];
    for (int i = 0; i < 2; i++) { items[i].label = "x"; items[i].icon = dummy; items[i].iw = 2; items[i].ih = 2; }
    nwui_node *iv = nwui_iconview(u, on_act, on_chg, &g_act);
    nwui_iconview_set(iv, items, 2);
    nwui_set_root(u, iv);
    u->win_w = 300; u->win_h = 300;
    nwui_layout(u);
    int cx = iv->x + NWUI_ICON_CELL_W / 2, cy = iv->y + NWUI_ICON_CELL_H / 2;
    u->now_ms = 1000; click(u, cx, cy);
    u->now_ms = 1100; click(u, cx, cy);     // within NWUI_DBL_MS
    CHECK(g_act == 1);
    delete u;
}
```

- [ ] **Step 2: Run it — expect FAIL (undefined `nwui_iconview`)**

Run: `make test` (after adding the file to the test list as in Task 2.4)
Expected: link error / undefined reference — confirming the test exercises new code.

- [ ] **Step 3: Implement the constructor + set + selected in `nwui_core.c`**

```c
nwui_node *nwui_iconview(nwui *u, nwui_cb on_activate, nwui_cb on_change, void *user)
{
    nwui_node *n = nwui_alloc(u, NWUI_ICONVIEW);
    n->on_click  = on_activate;     /* reuse on_click slot for activate, matching the list */
    n->on_change = on_change;
    n->user      = user;
    n->focusable = 1;
    n->sel       = -1;
    n->last_row  = -1;
    return n;
}

void nwui_iconview_set(nwui_node *n, const nwui_icon_item *items, int count)
{
    if (n->kind != NWUI_ICONVIEW) return;
    n->icons = items;
    n->count = count;
    if (n->sel >= count) n->sel = -1;
    n->scroll = 0;
    n->dirty = 1;
    if (n->owner) n->owner->layout_dirty = 1;
}

int nwui_iconview_selected(nwui_node *n)
{
    return (n && n->kind == NWUI_ICONVIEW) ? n->sel : -1;
}
```

- [ ] **Step 4: Add measure + arrange cases**

In `nwui_measure()` add a case so an iconview asks for a sensible default (it is normally `flex`'d to fill):

```c
    case NWUI_ICONVIEW:
        n->mw = NWUI_ICON_CELL_W * 2;        /* min 2 columns */
        n->mh = NWUI_ICON_CELL_H * 2;        /* min 2 rows */
        break;
```

In `nwui_arrange()` (or at the end of arrange for leaf widgets), compute the column count once the rect is known. If arrange doesn't have a leaf hook, compute `cols` lazily in dispatch/paint; simplest is to set it in arrange:

```c
    if (n->kind == NWUI_ICONVIEW) {
        int c = n->w / NWUI_ICON_CELL_W;
        n->cols = c < 1 ? 1 : c;
    }
```

- [ ] **Step 5: Add event handling (single-click selection + double-click activate + scroll)**

In `nwui_dispatch()`, alongside the `NWUI_LIST` pointer handling, add for an iconview hit on left-press-release. Mirror the list's double-click timing (`last_row`/`last_ms`/`NWUI_DBL_MS`) but compute the index from grid geometry:

```c
    if (over->kind == NWUI_ICONVIEW && over->cols > 0) {
        int relx = ev->x - over->x, rely = ev->y - over->y + over->scroll * NWUI_ICON_CELL_H;
        int col = relx / NWUI_ICON_CELL_W, row = rely / NWUI_ICON_CELL_H;
        int idx = (relx >= 0 && col < over->cols) ? row * over->cols + col : -1;
        if (idx >= 0 && idx < over->count) {
            over->sel = idx; over->dirty = 1;
            int dbl = (idx == over->last_row && u->now_ms - over->last_ms <= NWUI_DBL_MS);
            over->last_row = idx; over->last_ms = u->now_ms;
            if (dbl) { over->last_row = -1; if (over->on_click) over->on_click(over, over->user); }
            else     { if (over->on_change) over->on_change(over, over->user); }
        }
    }
```

Add Enter-to-activate where the list handles keys (when an iconview is focused and `sel >= 0`, call `on_click`). Add wheel/scroll handling mirroring the list if the list has it; clamp `scroll` to `max(0, rows - visible_rows)` where `rows = (count + cols - 1) / cols`.

- [ ] **Step 6: Run the test — expect PASS**

Run: `make test`
Expected: both iconview cases pass.

- [ ] **Step 7: Commit**

```bash
git add user/libnwui/nwui_core.h user/libnwui/nwui.h user/libnwui/nwui_core.c tests/test_nwui_iconview.cpp Makefile
git commit -m "feat(libnwui): NWUI_ICONVIEW grid widget (layout, selection, double-click)"
```

### Task 2.3: Paint the iconview

**Files:**
- Modify: `user/libnwui/nwui_paint.c`

- [ ] **Step 1: Add the paint case**

In `paint_self()` add, modeled on the `NWUI_LIST` case (clip to the node rect; scale/blit the icon centered in the cell's top area; draw the label centered below; highlight the selected cell with `COL_SEL`):

```c
case NWUI_ICONVIEW: {
    nw_fill_round(s, n->x, n->y, n->w, n->h, 8, COL_TF_BG, 255);
    int cols = n->cols < 1 ? 1 : n->cols;
    for (int i = 0; i < n->count; i++) {
        int row = i / cols, col = i % cols;
        int cx = n->x + col * NWUI_ICON_CELL_W;
        int cy = n->y + row * NWUI_ICON_CELL_H - n->scroll * NWUI_ICON_CELL_H;
        if (cy + NWUI_ICON_CELL_H < n->y || cy > n->y + n->h) continue;   /* off-screen */
        if (i == n->sel)
            nw_fill_round(s, cx + 4, cy + 2, NWUI_ICON_CELL_W - 8, NWUI_ICON_CELL_H - 4, 6, COL_SEL, 200);
        const nwui_icon_item *it = &n->icons[i];
        if (it->icon && it->iw > 0 && it->ih > 0) {
            int ix = cx + (NWUI_ICON_CELL_W - NWUI_ICON_PX) / 2, iy = cy + 8;
            nwui_blit_scaled(s, it->icon, it->iw, it->ih, ix, iy, NWUI_ICON_PX, NWUI_ICON_PX);
        }
        if (it->label)
            nw_text_centered(s, cx, cy + 8 + NWUI_ICON_PX + 4, NWUI_ICON_CELL_W, it->label,
                             (i == n->sel) ? 0x00ffffff : COL_INK);
    }
}
break;
```

- [ ] **Step 2: Provide the paint helpers used above**

If `nw_text_centered` / `nwui_blit_scaled` don't exist, add small static helpers in `nwui_paint.c`. A nearest-neighbour scaler:

```c
static void nwui_blit_scaled(const struct nw_surface *s, const uint32_t *src, int sw, int sh,
                             int dx, int dy, int dw, int dh) {
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw, sy = y * sh / dh;
            nw_put_pixel(s, dx + x, dy + y, src[sy * sw + sx]);   /* use the surface's pixel writer */
        }
}
```

For centered text, measure with the existing font width (`NW_FONT_W` or the helper nwui uses for the list) and offset; if the toolkit lacks a width metric, left-pad by `(cell - len*NW_FONT_W)/2`. Match whatever the existing paint code uses (`grep -n "nw_text\|NW_FONT" user/libnwui/nwui_paint.c`).

- [ ] **Step 3: Build the image — visual smoke comes in Phase 8**

Run: `make image64`
Expected: clean compile of `nwui_paint.c`.

- [ ] **Step 4: Commit**

```bash
git add user/libnwui/nwui_paint.c
git commit -m "feat(libnwui): paint NWUI_ICONVIEW (scaled icons + centered labels + selection)"
```

### Task 2.4: Register the iconview test module

**Files:** Modify `Makefile`

- [ ] **Step 1:** Add `tests/test_nwui_iconview.cpp` to the toolkit host-test source list (next to `test_nwui_core.cpp`); `nwui_core.c`/`nwui_paint.c` are already compiled under test. Ensure the coverage pattern covers the new code.
- [ ] **Step 2:** Run `make test`; expect green + coverage gate satisfied.
- [ ] **Step 3:** Commit: `git commit -am "test(libnwui): wire iconview tests into the host suite"`

---

## Phase 3 — `NWUI_PANEL` titled group-box (reuse: the XP task-pane building block)

### Task 3.1: Implement the panel widget

**Files:**
- Modify: `user/libnwui/nwui_core.c`, `user/libnwui/nwui_paint.c`
- Test: `tests/test_nwui_panel.cpp`

- [ ] **Step 1: Write the failing test (header reserves space; children stack below it)**

`tests/test_nwui_panel.cpp`:

```cpp
#include "doctest.h"
#include "nwui_core.h"
#include "nwui.h"
#include "libnw.h"

TEST_CASE("panel reserves a title header and stacks children beneath it") {
    nwui *u = new nwui; nwui_init(u);
    nwui_node *p = nwui_panel(u, "Other Places");
    nwui_node *a = nwui_label(u, "Home");
    nwui_node *b = nwui_label(u, "My Computer");
    nwui_add(p, a); nwui_add(p, b);
    nwui_set_root(u, p);
    u->win_w = 160; u->win_h = 200;
    nwui_layout(u);
    CHECK(a->y >= p->y + NWUI_PANEL_TITLE_H);   // below the header
    CHECK(b->y >  a->y);                         // stacked
    delete u;
}
```

- [ ] **Step 2: Run — expect FAIL (undefined `nwui_panel`)**

Run: `make test`
Expected: undefined reference.

- [ ] **Step 3: Implement the constructor**

In `nwui_core.c`, a panel is a vertical container whose title lives in `text` and which reserves a header band. Reuse the column layout but offset children by the header height:

```c
nwui_node *nwui_panel(nwui *u, const char *title)
{
    nwui_node *n = nwui_alloc(u, NWUI_PANEL);
    int i = 0;
    for (; title && title[i] && i < NWUI_TEXT_CAP - 1; i++) n->text[i] = title[i];
    n->text[i] = 0;
    n->pad = 8;
    n->gap = 4;
    return n;
}
```

- [ ] **Step 4: Measure + arrange like a column, plus the header**

In `nwui_measure()`: panel measures like a column (sum children heights + gaps + 2*pad) and adds `NWUI_PANEL_TITLE_H` to height; width = max child width + 2*pad. In `nwui_arrange()`: arrange children vertically starting at `y + NWUI_PANEL_TITLE_H + pad`, x at `x + pad`, width `w - 2*pad`, applying `gap`. (Copy the existing `NWUI_COLUMN` arrange branch and add the title offset to the starting `y` and the available height.)

- [ ] **Step 5: Paint the panel (glass tint + title)**

In `nwui_paint.c`:

```c
case NWUI_PANEL:
    nw_fill_round(s, n->x, n->y, n->w, n->h, 8, COL_PANEL_BG, 235);          /* glass body */
    nw_fill_round(s, n->x, n->y, n->w, NWUI_PANEL_TITLE_H, 8, COL_PANEL_HDR, 255); /* header band */
    nw_text(s, n->x + 8, n->y + (NWUI_PANEL_TITLE_H - NW_FONT_H) / 2, n->text, 0x00ffffff);
    break;   /* children are painted by the normal recursive walk */
```

Define `COL_PANEL_BG` / `COL_PANEL_HDR` next to the other `COL_*` constants in `nwui_paint.c`, picking a soft glass-blue consistent with the compositor accent (`grep -n "COL_SEL\|COL_TF_BG" user/libnwui/nwui_paint.c` for the existing palette and match its style).

- [ ] **Step 6: Run the test — expect PASS**

Run: `make test`

- [ ] **Step 7: Register the panel test + commit**

Add `tests/test_nwui_panel.cpp` to the host-test list. Then:

```bash
git add user/libnwui/nwui_core.c user/libnwui/nwui_paint.c tests/test_nwui_panel.cpp Makefile
git commit -m "feat(libnwui): NWUI_PANEL titled glass group-box (XP task-pane building block)"
```

---

## Phase 4 — Icon assets + on-disk install

### Task 4.1: Host icon generator + committed PNGs

**Files:**
- Create: `tools/gen-icons.py`, `assets/icons/{computer,drive,folder,home,program,text,image,file}.png`

- [ ] **Step 1: Write `tools/gen-icons.py`**

A stdlib-only generator (no PIL): draw each 48×48 icon at 4× (192×192) into an RGBA byte array with simple anti-aliased shapes, box-downsample to 48×48, and write a PNG via `zlib.compress` + manual chunks (reuse the chunk helper from Task 1.2 Step 1, extended to color type 6 / RGBA). Each icon is a small function returning a `bytearray`. Glass-style palette (blues/greys matching the desktop). Minimum viable shapes:

```python
#!/usr/bin/env python3
# tools/gen-icons.py — generate the libnwui 48x48 RGBA icon set (NanOS-glass style). Stdlib only.
import zlib, struct, os, math

S = 48
SS = 4                     # supersample factor
N = S * SS

def blank(): return [[(0,0,0,0)] * N for _ in range(N)]

def disk(buf, cx, cy, r, col):
    for y in range(N):
        for x in range(N):
            if (x-cx)**2 + (y-cy)**2 <= r*r:
                buf[y][x] = col

def rect(buf, x0, y0, x1, y1, col):
    for y in range(max(0,y0), min(N,y1)):
        for x in range(max(0,x0), min(N,x1)):
            buf[y][x] = col

def downsample(buf):
    out = bytearray()
    for Y in range(S):
        for X in range(S):
            r=g=b=a=0
            for dy in range(SS):
                for dx in range(SS):
                    pr,pg,pb,pa = buf[Y*SS+dy][X*SS+dx]
                    r+=pr; g+=pg; b+=pb; a+=pa
            n=SS*SS
            out += bytes((r//n, g//n, b//n, a//n))
        # PNG filter byte per row goes in front during encode, not here
    return out

def png_rgba(pixels_rows):  # pixels_rows: list of S rows, each S*4 bytes
    raw = b"".join(b"\x00" + row for row in pixels_rows)
    def chunk(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))

def emit(name, buf):
    flat = downsample(buf)
    rows = [flat[y*S*4:(y+1)*S*4] for y in range(S)]
    os.makedirs("assets/icons", exist_ok=True)
    open(f"assets/icons/{name}.png","wb").write(png_rgba(rows))
    print("wrote assets/icons/%s.png" % name)

GLASS=(70,130,200,255); GREY=(150,160,170,255); DARK=(40,60,90,255)
PAPER=(235,238,245,255); GREEN=(90,170,90,255); AMBER=(220,180,80,255)

def folder():
    b=blank(); rect(b, 20, 64, 172, 150, AMBER); rect(b, 20, 50, 90, 70, AMBER); return b
def drive():
    b=blank(); rect(b, 24, 60, 168, 132, GREY); rect(b, 36, 76, 156, 96, DARK); disk(b,150,116,6,GREEN); return b
def computer():
    b=blank(); rect(b, 28, 36, 164, 132, DARK); rect(b, 40, 48, 152, 120, GLASS); rect(b, 70, 140, 122, 160, GREY); return b
def home():
    b=blank()
    for y in range(40,96):                          # roof triangle
        half=(y-40); rect(b, 96-half, y, 96+half, y+1, GREEN)
    rect(b, 56, 96, 136, 156, PAPER); rect(b, 86, 120, 106, 156, DARK); return b
def program():
    b=blank(); rect(b, 40, 40, 152, 152, GLASS); rect(b, 56, 64, 136, 80, PAPER); rect(b,56,96,120,112,PAPER); return b
def text():
    b=blank(); rect(b, 48, 32, 144, 160, PAPER)
    for y in range(56,150,18): rect(b, 60, y, 132, y+4, GREY)
    return b
def image():
    b=blank(); rect(b, 40, 48, 152, 144, PAPER); disk(b,72,80,10,AMBER); rect(b,52,120,140,136,GLASS); return b
def generic():
    b=blank(); rect(b, 52, 32, 140, 160, PAPER); rect(b, 112, 32, 140, 60, GREY); return b

emit("folder", folder()); emit("drive", drive()); emit("computer", computer())
emit("home", home()); emit("program", program()); emit("text", text())
emit("image", image()); emit("file", generic())
```

- [ ] **Step 2: Generate and eyeball the icons**

Run: `python3 tools/gen-icons.py`
Then open `assets/icons/` and confirm 8 PNGs exist and look like recognizable shapes (open in Preview). Tweak the shape functions if any icon is illegible.

- [ ] **Step 3: Verify they decode through the toolkit loader (reuse the Phase 1 test harness ad hoc)**

Run: `python3 -c "import struct;f=open('assets/icons/folder.png','rb').read();print(len(f),f[:8])"`
Expected: a non-trivial size and the PNG magic `\x89PNG\r\n\x1a\n`.

- [ ] **Step 4: Commit**

```bash
git add tools/gen-icons.py assets/icons/*.png
git commit -m "feat(assets): NanOS-glass 48x48 icon set + stdlib PNG generator"
```

### Task 4.2: Install icons onto the disk image

**Files:** Modify `Makefile`

- [ ] **Step 1: Create `/nanos/share/icons` in `_image64`**

In the `_image64` target's `debugfs mkdir` line (the one that creates `/nanos/share`, found at the `mkdir /nanos/share` invocation), append `mkdir /nanos/share/icons`.

- [ ] **Step 2: Write each icon PNG into the image**

After the wallpaper-install block in `_image64`, add:

```make
	for ic in computer drive folder home program text image file; do \
	  printf "rm /nanos/share/icons/$$ic.png\nwrite assets/icons/$$ic.png /nanos/share/icons/$$ic.png\n" | debugfs -w "$(IMAGE64_GRUB2_PART)" 2>/dev/null; \
	done
```

- [ ] **Step 3: Build the image and confirm the icons landed**

Run: `make image64` then dump the dir:

```bash
debugfs -R "ls -l /nanos/share/icons" disk/image64.img 2>/dev/null || \
  printf "ls /nanos/share/icons\n" | debugfs disk/image64.img
```

(Run inside the build container if `debugfs` isn't on the host — `make image64` already shells into it.)
Expected: 8 PNG entries.

- [ ] **Step 4: Commit**

```bash
git add Makefile
git commit -m "build: install the icon set to /nanos/share/icons in the disk image"
```

---

## Phase 5 — x86_64 Rust tooling (target + safe bindings + a trivial validation app)

### Task 5.1: Add the x86_64 Rust target spec

**Files:** Create `user/rust/x86_64-nanos.json`

- [ ] **Step 1: Write the target (the 64-bit sibling of `user/rust/i686-nanos.json`)**

```json
{
  "llvm-target": "x86_64-unknown-none",
  "data-layout": "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128",
  "arch": "x86_64",
  "target-endian": "little",
  "target-pointer-width": 64,
  "target-c-int-width": 32,
  "os": "none",
  "vendor": "unknown",
  "executables": true,
  "linker-flavor": "ld",
  "linker": "x86_64-elf-ld",
  "relocation-model": "static",
  "code-model": "small",
  "panic-strategy": "abort",
  "disable-redzone": true,
  "features": "-mmx,-sse,-sse2,-sse3,-ssse3,-sse4.1,-sse4.2,-avx,-avx2,+soft-float",
  "dynamic-linking": false,
  "position-independent-executables": false,
  "has-thread-local": false
}
```

Note: confirm the cross `ld` name with `grep -n "x86_64-elf-ld\|LD=" Makefile`; use whatever the x64 `LD` variable resolves to. The `code-model` must match how the x64 userland is linked (the `user-nx.ld` base is `0x800000`, so `small`/`static` is correct).

- [ ] **Step 2: Commit**

```bash
git add user/rust/x86_64-nanos.json
git commit -m "build(rust): x86_64-nanos bare target spec (soft-float, static, LP64)"
```

### Task 5.2: The `libnwui-rs` safe bindings crate

**Files:** Create `user/rust/libnwui-rs/Cargo.toml`, `user/rust/libnwui-rs/src/lib.rs`

- [ ] **Step 1: `Cargo.toml`**

```toml
[package]
name = "libnwui-rs"
version = "0.1.0"
edition = "2021"

[lib]
path = "src/lib.rs"
crate-type = ["rlib"]
```

- [ ] **Step 2: `src/lib.rs` — runtime shim + FFI + safe wrappers**

Lift the runtime/allocator/panic + FFI pattern from `user/rust/rustform/src/nanos.rs` and extend it with the explorer's needs (iconview, panel, image load, menus, spawn). Key surface:

```rust
#![no_std]
extern crate alloc;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;
use core::panic::PanicInfo;
use alloc::boxed::Box;
use alloc::vec::Vec;

extern "C" {
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
    pub fn exit(code: i32) -> !;
    fn write(fd: i32, p: *const u8, n: usize) -> isize;
}
struct Libc;
unsafe impl GlobalAlloc for Libc {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { malloc(l.size().max(1)) }
    unsafe fn dealloc(&self, p: *mut u8, _l: Layout) { free(p) }
}
#[global_allocator] static ALLOC: Libc = Libc;
#[panic_handler] fn on_panic(_: &PanicInfo) -> ! {
    let m = b"rust: panic\n"; unsafe { write(2, m.as_ptr(), m.len()); exit(101) }
}

#[repr(C)] pub struct NwUi { _o: [u8; 0] }
#[repr(C)] pub struct NwNode { _o: [u8; 0] }
#[repr(C)] pub struct IconItem { pub label: *const u8, pub icon: *const u32, pub iw: i32, pub ih: i32 }
type RawCb = extern "C" fn(*mut NwNode, *mut c_void);

extern "C" {
    fn nwui_open(t: *const u8, w: i32, h: i32) -> *mut NwUi;
    fn nwui_set_root(u: *mut NwUi, r: *mut NwNode);
    fn nwui_run(u: *mut NwUi);
    fn nwui_label(u: *mut NwUi, t: *const u8) -> *mut NwNode;
    fn nwui_button(u: *mut NwUi, t: *const u8, cb: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_panel(u: *mut NwUi, t: *const u8) -> *mut NwNode;
    fn nwui_iconview(u: *mut NwUi, act: RawCb, chg: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_iconview_set(n: *mut NwNode, items: *const IconItem, count: i32);
    fn nwui_iconview_selected(n: *mut NwNode) -> i32;
    fn nwui_vbox(u: *mut NwUi) -> *mut NwNode;
    fn nwui_hbox(u: *mut NwUi) -> *mut NwNode;
    fn nwui_add(p: *mut NwNode, c: *mut NwNode) -> *mut NwNode;
    fn nwui_pad(n: *mut NwNode, p: i32) -> *mut NwNode;
    fn nwui_gap(n: *mut NwNode, g: i32) -> *mut NwNode;
    fn nwui_flex(n: *mut NwNode, f: i32) -> *mut NwNode;
    fn nwui_set_text(n: *mut NwNode, t: *const u8);
    fn nwui_spawn(u: *mut NwUi, cmd: *const u8);
    fn nwui_image_load_png(path: *const u8, w: *mut i32, h: *mut i32) -> *mut u32;
}

pub fn cstr(s: &str) -> Vec<u8> { let mut v = Vec::with_capacity(s.len()+1); v.extend_from_slice(s.as_bytes()); v.push(0); v }

#[derive(Clone, Copy)] pub struct Node(pub *mut NwNode);
impl Node {
    pub fn pad(self, p: i32) -> Node { unsafe { Node(nwui_pad(self.0, p)) } }
    pub fn gap(self, g: i32) -> Node { unsafe { Node(nwui_gap(self.0, g)) } }
    pub fn flex(self, f: i32) -> Node { unsafe { Node(nwui_flex(self.0, f)) } }
    pub fn add(self, c: Node) -> Node { unsafe { nwui_add(self.0, c.0); } self }
    pub fn set_text(self, s: &str) { let c = cstr(s); unsafe { nwui_set_text(self.0, c.as_ptr()) } }
    pub fn iconview_set(self, items: &[IconItem]) { unsafe { nwui_iconview_set(self.0, items.as_ptr(), items.len() as i32) } }
    pub fn iconview_selected(self) -> i32 { unsafe { nwui_iconview_selected(self.0) } }
}

extern "C" fn trampoline(_n: *mut NwNode, user: *mut c_void) {
    unsafe { let f = &mut *(user as *mut Box<dyn FnMut()>); f(); }
}

pub struct Ui(pub *mut NwUi);
impl Ui {
    pub fn open(title: &str, w: i32, h: i32) -> Option<Ui> {
        let t = cstr(title); let p = unsafe { nwui_open(t.as_ptr(), w, h) };
        if p.is_null() { None } else { Some(Ui(p)) }
    }
    pub fn label(&self, s: &str) -> Node { let c = cstr(s); unsafe { Node(nwui_label(self.0, c.as_ptr())) } }
    pub fn panel(&self, title: &str) -> Node { let c = cstr(title); unsafe { Node(nwui_panel(self.0, c.as_ptr())) } }
    pub fn vbox(&self) -> Node { unsafe { Node(nwui_vbox(self.0)) } }
    pub fn hbox(&self) -> Node { unsafe { Node(nwui_hbox(self.0)) } }
    pub fn button<F: FnMut() + 'static>(&self, s: &str, f: F) -> Node {
        let boxed: Box<dyn FnMut()> = Box::new(f);
        let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
        let c = cstr(s); unsafe { Node(nwui_button(self.0, c.as_ptr(), trampoline, user)) }
    }
    pub fn iconview<F: FnMut() + 'static, G: FnMut() + 'static>(&self, on_act: F, on_chg: G) -> Node {
        let a: Box<dyn FnMut()> = Box::new(on_act); let g: Box<dyn FnMut()> = Box::new(on_chg);
        // Two callbacks need two user pointers; store both in a small struct and use two trampolines,
        // OR (simpler) keep activate via trampoline and change via a separate trampoline2 with its own box.
        let ua = Box::into_raw(Box::new(a)) as *mut c_void;
        let ug = Box::into_raw(Box::new(g)) as *mut c_void;
        unsafe { Node(nwui_iconview(self.0, trampoline, trampoline_chg, mux(ua, ug))) }
    }
    pub fn spawn(&self, cmd: &str) { let c = cstr(cmd); unsafe { nwui_spawn(self.0, c.as_ptr()) } }
    pub fn run(&self, root: Node) { unsafe { nwui_set_root(self.0, root.0); nwui_run(self.0) } }
}

pub fn load_png(path: &str) -> Option<(*mut u32, i32, i32)> {
    let c = cstr(path); let mut w = 0i32; let mut h = 0i32;
    let p = unsafe { nwui_image_load_png(c.as_ptr(), &mut w, &mut h) };
    if p.is_null() { None } else { Some((p, w, h)) }
}
```

Note on the two-callback iconview: the C API passes a single `user` pointer to BOTH callbacks. The simplest robust binding is to make the C `nwui_iconview` take **one** `user` and have the app store its state there; expose the activate/change distinction by having `libnwui-rs` keep one boxed closure for activate and read the selection inside it (the change callback can be a no-op closure if the app only needs activate). **Decision for the binding:** keep ONE `user` pointer and ONE boxed closure that the app uses for activate; pass `core::ptr::null` for the change trampoline when the app doesn't need live selection feedback. The `mux`/`trampoline_chg` sketch above is optional; prefer the single-closure form to avoid leaking two boxes. Update the C `nwui_iconview` signature to a single callback if the two-callback split proves awkward — but the spec's Details panel needs change notification, so keep both in C and, in Rust, store a single app-state pointer that both trampolines receive.

- [ ] **Step 3: Commit**

```bash
git add user/rust/libnwui-rs/
git commit -m "feat(rust): libnwui-rs safe bindings crate (window, iconview, panel, image, spawn)"
```

### Task 5.3: Prove the x64 Rust toolchain links a real `.nxe` (de-risk before the app)

**Files:** Create `user/rust/rsexp/Cargo.toml`, `user/rust/rsexp/src/lib.rs` (minimal); Modify `Makefile`

- [ ] **Step 1: Minimal app — a window with one label**

`user/rust/rsexp/Cargo.toml`:

```toml
[package]
name = "rsexp"
version = "0.1.0"
edition = "2021"

[lib]
path = "src/lib.rs"
crate-type = ["staticlib"]

[dependencies]
libnwui-rs = { path = "../libnwui-rs" }

[profile.release]
panic = "abort"
opt-level = "z"
[profile.dev]
panic = "abort"
```

`user/rust/rsexp/src/lib.rs` (minimal first cut):

```rust
#![no_std]
extern crate alloc;
use libnwui_rs::Ui;

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let ui = match Ui::open("Files (Rust)", 480, 320) { Some(u) => u, None => return 1 };
    let root = ui.vbox().add(ui.label("My Computer")).pad(12).gap(8);
    ui.run(root);
    0
}
```

- [ ] **Step 2: Add the Makefile build rule (mirror `rustform` + the x64 `nwexp` link line)**

After the `rustform` rules, add:

```make
RSEXP_TARGET=user/rust/x86_64-nanos.json
RSEXP_LIB=user/rust/rsexp/target/x86_64-nanos/release/librsexp.a
$(RSEXP_LIB): user/rust/rsexp/src/lib.rs user/rust/libnwui-rs/src/lib.rs user/rust/rsexp/Cargo.toml $(RSEXP_TARGET)
	cd user/rust/rsexp && cargo build -Z build-std=core,alloc -Z json-target-spec --target ../x86_64-nanos.json --release
$(BINFOLDER)rsexp.nxe: $(DYN_GLUE) $(RSEXP_LIB) $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a $(BINFOLDER)libnwui.ndl $(BINFOLDER)libnw.ndl $(BINFOLDER)libc.ndl $(MKNX_TOOL)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(USER_NX_LD) -o $(BINFOLDER)rsexp.elf $(DYN_GLUE) $(RSEXP_LIB) $(BINFOLDER)libnwui.ndl.a $(BINFOLDER)libc.ndl.a -lgcc
	$(MKNX_TOOL) $(BINFOLDER)rsexp.elf $@ --need libnwui.ndl
```

Guard the rule so it only applies for `ARCH=x86_64` (the GUI is x64). Verify `cargo` is available in the build container (the i686 `rustform` already proves the toolchain is installed; if the container lacks the x64 `rust-std` source for build-std, `-Z build-std` rebuilds core/alloc from source, which only needs the `rust-src` component — confirm with `rustup component list` / `cargo build -Z build-std` succeeding for i686 today).

- [ ] **Step 3: Build just the binary**

Run: `make bin/rsexp.nxe` (or the arch-qualified equivalent; if the target name differs, `grep -n "rustform.nxe" Makefile` to see how single-app builds are invoked).
Expected: `cargo` compiles `librsexp.a`, `ld` links, `mknx` emits `bin/rsexp.nxe` with no undefined symbols.

- [ ] **Step 4: Commit**

```bash
git add user/rust/rsexp/Cargo.toml user/rust/rsexp/src/lib.rs Makefile
git commit -m "feat(rust): x64 rsexp skeleton .nxe links against libnwui-rs (toolchain de-risk)"
```

---

## Phase 6 — The explorer app (Rust) + desktop integration

### Task 6.1: My Computer + folder model and views

**Files:** Modify `user/rust/rsexp/src/lib.rs`

- [ ] **Step 1: Implement the model + directory enumeration via libc FFI**

Extend `libnwui-rs` (or add a small `fs` module in the app) with FFI to `opendir`/`readdir`/`closedir` and `getenv`, mirroring how the C `nwexp` reads directories. Define:

```rust
enum Location { MyComputer, Path(alloc::string::String) }

struct Entry { name: alloc::string::String, is_dir: bool, kind: IconKind }
enum IconKind { Computer, Drive, Folder, Home, Program, Text, Image, File }
```

`load_my_computer()` opens `/disks`, makes one `Drive` entry per child (label = volume name, path `/disks/<name>`), then appends a `Home` entry (`getenv("HOME")` or `/disks/main`). `load_folder(path)` reads the dir, classifies each entry by `d_type == DT_DIR` and extension (`.nxe`→Program, `.txt/.c/.h/.md/.cfg/.rs`→Text, `.png`→Image, else File), and prepends `..` unless at a disk-mount root (where `..` returns to MyComputer).

- [ ] **Step 2: Pre-load the 8 icons once at startup**

```rust
struct Icons { computer:(*mut u32,i32,i32), drive:.., folder:.., home:.., program:.., text:.., image:.., file:.. }
fn load_icons() -> Icons { /* libnwui_rs::load_png("/disks/main/nanos/share/icons/<k>.png"), fallback to file icon on None */ }
```

Build the `IconItem` array from the current entries, mapping each `IconKind` to the preloaded pixel buffer + dims. Call `iconview_set` with that slice (keep the `Vec<IconItem>` and the label `Vec<Vec<u8>>` alive for the iconview's lifetime — the C side holds the pointers).

- [ ] **Step 3: Build the binary**

Run: `make bin/rsexp.nxe`
Expected: compiles; no runtime test yet (Phase 8 verifies on screen).

- [ ] **Step 4: Commit**

```bash
git add user/rust/rsexp/src/lib.rs user/rust/libnwui-rs/src/lib.rs
git commit -m "feat(rsexp): My Computer + folder model, entry classification, icon preload"
```

### Task 6.2: Wire the window — toolbar, task pane, iconview, navigation

**Files:** Modify `user/rust/rsexp/src/lib.rs`

- [ ] **Step 1: Compose the layout**

```
vbox(pad 8, gap 6):
  hbox(toolbar): [Up] [Home]  + breadcrumb label
  hbox(flex 1):
    vbox(width ~150): panel("Other Places") { Home, My Computer, Up }
                      panel("Details")      { name label, kind label }
    flex 1: iconview(on_activate, on_change)
```

The activate closure reads `iconview_selected()`, maps to the current entry, and: enters a dir (`load_folder` + refresh), spawns a `.nxe` (`ui.spawn(absolute_path)`), or returns to MyComputer (on `..` at a mount root). The change closure updates the Details panel labels via `Node::set_text`. Up/Home toolbar buttons and the "Other Places" links call the same navigation functions.

Because the iconview's items are app-owned, keep app state (current `Location`, the `Vec<Entry>`, the `Vec<IconItem>`, the live label buffers, and the `Node` handles for the breadcrumb + Details labels) in a single heap struct whose pointer is the iconview/buttons' `user` — leak it with `Box::into_raw` for the app's lifetime (single-process, no teardown needed).

- [ ] **Step 2: Handle the no-arg vs path-arg entry**

`main` checks `argc`/`argv` (FFI to read them, or a libc `__argc/__argv` shim if crt0 provides them; otherwise accept the convention that no arg ⇒ MyComputer). If a path arg is present, start in `Location::Path(arg)`, else `Location::MyComputer`.

- [ ] **Step 3: Build + commit**

Run: `make bin/rsexp.nxe`

```bash
git add user/rust/rsexp/src/lib.rs
git commit -m "feat(rsexp): XP layout — toolbar, Other Places + Details task pane, icon grid, navigation"
```

### Task 6.3: Swap rsexp in for nwexp in the image

**Files:** Modify `Makefile`

- [ ] **Step 1: Replace `nwexp` with `rsexp` in `X64_GUI_APPS`**

Find `X64_GUI_APPS=nwexp settings about notepad form terminal` and change to `X64_GUI_APPS=rsexp settings about notepad form terminal`. The `_image64` app-install loop (the `for p in $(X64_GUI_APPS)` block) then installs `bin/rsexp.nxe` → `/apps/rsexp/rsexp.nxe` and symlinks `/bin/rsexp.nxe`. Ensure `bin/rsexp.nxe` is a build prerequisite of `image64` (add it to the x64 app object/nxe list the same way `nwexp.nxe` was — `grep -n "nwexp.nxe" Makefile` and replace those prerequisite references with `rsexp.nxe`).

- [ ] **Step 2: Keep `nwexp` source for now (don't delete)**

Leave `user/nwexp/` in the tree (still buildable for i686), just out of the x64 desktop. Removing it is out of scope.

- [ ] **Step 3: Build the image**

Run: `make image64`
Expected: `rsexp.nxe` built and installed; `/bin/rsexp.nxe` symlink present.

- [ ] **Step 4: Commit**

```bash
git add Makefile
git commit -m "build: ship rsexp as the desktop Files app (replaces nwexp in X64_GUI_APPS)"
```

### Task 6.4: Add "My Computer" to the nwm Start menu

**Files:** Modify `user/nwm/nwm_core.c`

- [ ] **Step 1: Extend `LOGO_ITEMS` + bump the count**

```c
static const char *const LOGO_ITEMS[5] = { "My Computer", "About This Computer", "Run...", "Shut Down", "Quit" };
enum { LOGO_NITEMS = 5 };
```

- [ ] **Step 2: Spawn the explorer when "My Computer" is chosen**

In the logo-menu selection handler (where the existing items act — `grep -n "LOGO_ITEMS\|logo_select\|About This Computer" user/nwm/nwm_core.c`), add a branch for index 0 that sets the pending spawn to the explorer, reusing the same mechanism the Run dialog uses:

```c
/* index 0 => My Computer: launch the explorer with no path arg (opens the My Computer view) */
const char *cmd = "/bin/rsexp.nxe";
int i = 0; for (; cmd[i] && i < (int) sizeof s->run_cmd - 1; i++) s->run_cmd[i] = cmd[i];
s->run_cmd[i] = 0; s->want_spawn = 1;
```

Adjust the indices of the other items' handlers (they shifted by one now that "My Computer" is index 0).

- [ ] **Step 3: Build the image**

Run: `make image64`
Expected: clean compile of `nwm_core.c`.

- [ ] **Step 4: Commit**

```bash
git add user/nwm/nwm_core.c
git commit -m "feat(nwm): My Computer entry in the Start menu launches the explorer"
```

---

## Phase 7 — Full host-test + check-arch gate

### Task 7.1: Run the complete host suite and the arch leak check

- [ ] **Step 1:** Run `make check-arch` — expect clean (no x86 internals leaked into MI/userland).
- [ ] **Step 2:** Run `make test` and `make test64` — expect all toolkit tests (core + iconview + panel + png) green and the coverage gate satisfied. If coverage dropped below the gate for `nwui_core.c`/`nwui_paint.c`/`nwui_png.c`, add cases (e.g. iconview scroll clamp, panel with zero children) until ≥ the gate.
- [ ] **Step 3:** Commit any added tests: `git commit -am "test(libnwui): cover iconview scroll clamp + panel edge cases"`

---

## Phase 8 — QEMU end-to-end visual verification

### Task 8.1: Boot and screenshot the explorer

- [ ] **Step 1: Build the desktop image**

Run: `make image64`

- [ ] **Step 2: Boot headless with a monitor socket (per CLAUDE.md)**

Use the project's headless pattern: set `grub.cfg` `timeout=0`, boot `qemu-system-x86_64 ... -drive file=disk/image64.img,format=raw -display none -monitor unix:/tmp/qmon,server,nowait`, log in if needed, switch to the graphics VT (F7), open the Start menu → "My Computer", `screendump /tmp/x.ppm` via the monitor socket, `sips -s format png /tmp/x.ppm --out /tmp/x.png`, and read `/tmp/x.png`.

- [ ] **Step 3: Verify each acceptance criterion from the screenshots**

Confirm, capturing a screendump at each step:
- My Computer shows one **drive icon per `/disks/*` volume** + a **Home** icon, with the **Other Places** and **Details** task-pane panels visible.
- Double-clicking `main` enters `/disks/main`; folders/`.nxe`/text/image/other show their **distinct icons**.
- Single-click updates the **Details** panel (name + kind).
- **Up** returns toward My Computer; **Home** jumps to the home dir.
- Double-clicking a `.nxe` (e.g. `/bin/about.nxe` target) **spawns** it.
- nwm **wallpaper still renders** (PNG-decoder dedup regression check).

- [ ] **Step 4: Restore `grub.cfg` (timeout=5) and commit any fixes**

```bash
git checkout grub.cfg   # or restore timeout if edited in place
git commit -am "fix(rsexp): <whatever the screendump pass surfaced>"   # only if fixes were needed
```

### Task 8.2: Finish the branch

- [ ] **Step 1:** Ensure `make test64` is green and the working tree is clean.
- [ ] **Step 2:** Announce and invoke **superpowers:finishing-a-development-branch** to verify tests, present merge/PR options, and clean up. Do **not** push or merge without explicit user choice.

---

## Self-review notes (coverage against the spec)

- Spec A1 (PNG loader) → Phase 1. A2 (iconview) → Phase 2. A3 (panel) → Phase 3. A4 (icons+generator) → Phase 4.
- Spec B1 (x64 target) → Task 5.1. B2 (libnwui-rs) → Task 5.2. B3 (build wiring) → Tasks 5.3 + 6.3.
- Spec C (explorer app: model, My Computer, folder view, task pane, navigation) → Phase 6.1–6.2.
- Spec D (Start menu) → Task 6.4.
- Testing (host tests, check-arch, QEMU) → Phases 7–8.
- Decision 4 (XP grid + task pane, Up/Home only) and Decision 5 (NanOS glass) are reflected in the panel palette (Task 3.1 Step 5) and layout (Task 6.2).
- Decision 8 / the user's standing rule (reusable → libnwui): the PNG loader, iconview, and panel are all in the toolkit; the Rust app composes them.

**Open implementation risk to watch (flagged, not deferred):** the two-callback iconview binding in Rust (Task 5.2 Step 2) — resolve by keeping a single app-state pointer that both C trampolines receive; validate selection-change updates the Details panel during the Phase 8 screendump pass.
