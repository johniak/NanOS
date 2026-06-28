# Rust Icon File Explorer (XP-style, "My Computer") + Reusable Toolkit Growth — Design

**Date:** 2026-06-27
**Status:** Approved (brainstorming → spec)
**Supersedes:** `2026-06-25-rust-icon-explorer-design.md` (drafted on the now-stale
`feat/rust-icon-explorer` branch). This revision adds the Windows-XP **left task pane**, locks the
**NanOS-glass** aesthetic, and is rebased onto current `main` (so it keeps the recent Dell fixes:
HWP, meminfo, per-CPU execve, xHCI kernel-CR3 — the stale branch would have reverted them).

## Goal

A beautiful, **Windows-XP-style file explorer written from scratch in Rust**, running on the
x86_64 NanOS desktop (nwm). It has a **large-icon grid view**, **PNG icons loaded from disk**, the
classic XP **left task pane** ("Other Places" links + a "Details" box), and a virtual **"My
Computer"** root that lists each mounted disk under `/disks` **separately** plus the user's **Home**.
It replaces the existing C `nwexp` as the desktop's Files app and gains a **"My Computer" entry in
the nwm Start (logo) menu**. Every reusable piece lands in the shared toolkit (`libnwui`); the Rust
app is a thin client over it.

## Background / current state (verified against the tree)

- The GUI desktop is **x86_64** (`make image64`; `X64_GUI_PROGS=nwm login`,
  `X64_GUI_APPS=nwexp settings about notepad form terminal`). All GUI apps link the **dynamic**
  `libnwui.ndl` + `libc.ndl` via the C ABI, built with the x64 `crt0`+`nxhdr` glue (`DYN_GLUE`),
  `arch/x86_64/user-nx.ld`, and the x64 `mknx` (`MKNX64`). Reference rule: `nwexp.nxe` /
  `notepad.nxe` in the Makefile.
- The **only** Rust userland target today is **`user/rust/i686-nanos.json`** (32-bit). The existing
  `rustform` demo is a `no_std` cargo `staticlib` (`-Z build-std=core,alloc`) linked against the
  C-ABI `libnwui` — but **i686 only**, and **not** in the x64 desktop. Closing that tooling gap (an
  x64 Rust target + build wiring) is part of this work.
- `libnwui` (`user/libnwui/`: `nwui.h`, `nwui_core.{h,c}`, `nwui_paint.c`, `nwui.c`) has a
  **text-list** widget (`NWUI_LIST`) and a **static-image** widget (`NWUI_IMAGE`), but **no icon-grid
  view, no titled panel/sidebar, and no icon set**. Node kinds live in an enum in `nwui_core.h`; a
  widget = enum value + constructor (`nwui_core.c`) + measure/arrange + paint case (`nwui_paint.c`) +
  event case (`nwui_dispatch`). The list already has selection (`sel`), scroll (`scroll`), scrollbar
  drag, and double-click timing (`last_row`/`last_ms`, `NWUI_DBL_MS=400`) we mirror for the grid. The
  toolkit has a doctest host harness (`tests/test_nwui_core.cpp`).
- **nwm already ships a self-contained, from-scratch PNG decoder**: `user/nwm/png.{c,h}` —
  `uint32_t *png_decode(const uint8_t *data, unsigned len, int *w, int *h)` returning a malloc'd
  `0x00RRGGBB` buffer (non-interlaced 8-bit; grey/RGB/palette/grey+α/RGBA with α ignored; NULL on
  failure). nwm decodes `/disks/main/nanos/share/wallpaper.png` at runtime. **No libpng/zlib** — we
  promote this decoder into `libnwui`.
- The existing C explorer `user/nwexp/nwexp.c` is a text-list browser (opendir/readdir, `..` to
  climb, double-click a `.nxe` → `nwui_spawn`). It is the behavioral reference to evolve.
- The nwm Start menu is the top-left **logo menu**: `LOGO_ITEMS = {"About This Computer", "Run...",
  "Shut Down", "Quit"}`, `LOGO_NITEMS=4` (`user/nwm/nwm_core.c`). Apps launch via the pending-spawn
  slot (`want_spawn` / `nw_run_take_spawn`), the same path the Run dialog uses.
- `/disks` is a SynthFs directory listing the mounted volumes (e.g. `main`), enumerable from
  userland with opendir/readdir — so "My Computer" needs **no kernel change**.
- `/nanos/share` already exists on the image (created in `_image64`, holds `wallpaper.png`,
  `logo.raw`, `terminfo/`). Assets are installed via `debugfs write`. Adding
  `/nanos/share/icons/` + the PNGs follows that exact pattern.

## Decisions (locked during brainstorming)

1. **Icons:** real **PNG files** on disk under `/nanos/share/icons/`, decoded at runtime.
2. **PNG decoder:** **promote nwm's from-scratch `png.c`/`png.h` into `libnwui`** (zero new
   dependencies) rather than wiring the libpng/zlib SDK ports into the dynamic `.ndl` world.
3. **View modes:** **large-icon grid only** (List/Details deferred).
4. **Layout:** **XP icon grid + left task pane** — a blue-tinted sidebar with an **"Other Places"**
   panel (clickable links: Home, My Computer / disks) and a **"Details"** panel (selection name +
   kind/size). Toolbar = **Up** + **Home**. **No** Back/Forward history or editable address bar
   (deferred; an Up/Home toolbar + breadcrumb label is enough).
5. **Aesthetic:** **NanOS glass** — XP *layout and behavior*, but icons, panel tint, and selection
   accent match the existing glass-blur compositor theme (consistent with the rest of the OS). Not a
   pixel-accurate Luna reskin.
6. **Old explorer:** the Rust explorer **replaces** the C `nwexp` in `X64_GUI_APPS`.
7. **"My Computer":** a **userland-synthetic** view inside the explorer (enumerate `/disks/*` as
   separate drive items + a Home item). No kernel change.
8. **Reusable pieces → `libnwui` (C):** the PNG image-load module, the **icon-grid** widget, a
   reusable **titled panel / group-box** widget (the building block of the task pane), and the icon
   asset set. Per the user's standing instruction, **anything repeatable/reusable goes into the
   toolkit**, not the app. The Rust app composes these.
9. **App language:** the explorer is **written from scratch in Rust** on the **x86_64** desktop —
   requiring a new x64 Rust target + build wiring + a reusable safe binding crate.
10. **Start menu:** add a **"My Computer"** item to the nwm logo menu that launches the explorer.

## Architecture

### A. Reusable additions to `libnwui` (C) — the toolkit grows, the app stays thin

**A1. Image module — `nwui_image_load_png`.**
Lift `user/nwm/png.{c,h}` into `libnwui` (`user/libnwui/nwui_png.{c,h}`). Add a file-reading wrapper
that reads the file via libc into a heap buffer, calls `png_decode`, and returns a malloc'd
`0x00RRGGBB` buffer + `w`/`h` (caller frees). Public API in `nwui.h`:

```c
/* Load a PNG file into a malloc'd w*h 0x00RRGGBB buffer (caller frees). 0 on any failure. */
uint32_t *nwui_image_load_png(const char *path, int *w, int *h);
```

nwm is refactored to call the toolkit decoder (its `png.c` becomes a thin include/forward, or is
removed) so there is **one** decoder.

**A2. Icon-grid view widget — `NWUI_ICONVIEW`.**
A new node kind laid out as a **wrapping grid of icon-cells**: each cell = an icon (target 48×48)
centered above a 1–2 line label, with a selection highlight, hover feedback, vertical scrolling, and
double-click / Enter to activate. Reuses the list widget's selection / double-click-timing /
scrollbar machinery. Painted in `nwui_paint.c` alongside `NWUI_LIST`.

```c
/* One cell: a label + an icon (app-owned w*h 0x00RRGGBB buffer; may be shared across cells). */
typedef struct { const char *label; const uint32_t *icon; int iw, ih; } nwui_icon_item;

nwui_node *nwui_iconview(nwui *u, nwui_cb on_activate, void *user);
void       nwui_iconview_set(nwui_node *n, const nwui_icon_item *items, int count);
int        nwui_iconview_selected(nwui_node *n);
```

Cells flow left-to-right and wrap; cell width/height are fixed constants (`NWUI_ICON_CELL_W/H`); the
grid computes columns from content width and scrolls when rows overflow. To detect a selection
change (so the app updates the Details panel), the iconview fires `on_change` on single-click and
`on_activate` on double-click/Enter (mirror the list's two-callback pattern).

**A3. Titled panel / group-box widget — `NWUI_PANEL`.**
The reusable building block of the XP task pane: a rounded, glass-tinted container with a **title
bar** and a vertical content area into which the app adds children (link buttons, labels). Generic —
any app can build a sidebar/inspector from it.

```c
/* A titled panel (glass group-box). Add children with nwui_add(); they stack vertically. */
nwui_node *nwui_panel(nwui *u, const char *title);
```

The "Other Places" and "Details" boxes are each an `nwui_panel`; their contents are existing
primitives (link-style `nwui_button`s, `nwui_label`s). No app-specific chrome leaks out of the app.

**A4. Icon asset set (real PNGs).**
A small, coherent 48×48 RGBA icon set in a **NanOS-glass** style: `computer`, `drive`, `folder`,
`home`, `program` (`.nxe`), `text`, `image`, `file` (generic). Authored on the macOS host by a
**committed Python generator** (`tools/gen-icons.py`) using only the stdlib (`zlib` for PNG IDAT),
drawing anti-aliased shapes via supersampling — **no Docker rasterizer dependency**. Generated PNGs
are committed under `assets/icons/` and installed to **`/nanos/share/icons/`** by the
`_image`/`_image64` targets.

### B. Rust userland tooling upgrade

**B1. x86_64 Rust target.** New `user/rust/x86_64-nanos.json`, the 64-bit sibling of the i686 spec:
`llvm-target` `x86_64-unknown-none`, pointer-width 64, LP64 data layout, SSE/AVX disabled +
soft-float to match the kernel/userland ABI, `relocation-model` static, `code-model` small/kernel,
`panic-strategy` abort, `disable-redzone` true, the x64 `ld` linker.

**B2. Reusable safe binding crate `libnwui-rs`.** Under `user/rust/libnwui-rs/`: a `no_std` crate
exposing idiomatic safe Rust over the libnwui C ABI — window open/set_root/run, iconview
create/set/selected, panel, image load, labels/buttons, menus + items, spawn — plus the
runtime/allocator/panic shim (lifted from `rustform/src/nanos.rs`). The explorer depends on it so the
app is real Rust, not a wall of `unsafe extern`. Reusable by future Rust GUI apps.

**B3. Build wiring into `make image64`.** A cargo `staticlib` (`-Z build-std=core,alloc`, `--target
user/rust/x86_64-nanos.json`, `--release`) producing `librsexp.a`, linked with the **x64** `DYN_GLUE`
+ `arch/x86_64/user-nx.ld` + `libnwui.ndl.a` + `libc.ndl.a` through `MKNX64` (`--need libnwui.ndl`),
mirroring the i686 `rustform` rule and the x64 `nwexp` link line. The `.nxe` is added to
`X64_GUI_APPS`; `nwexp` is removed from it.

### C. The explorer app (Rust, from scratch)

Crate under `user/rust/rsexp/` (working name), a thin client over `libnwui-rs`.

- **Model.** `Location::MyComputer | Location::Path(String)`.
- **My Computer view.** Enumerate `/disks/*` (each volume → a `drive` icon, label = volume name) plus
  a **Home** item (`$HOME`, falling back to `/disks/main`, `home` icon). Double-click a drive enters
  its mount path (`/disks/<name>`); Home enters the home dir.
- **Folder view.** `readdir` the path; classify each entry — directory / `.nxe` program / text
  (`.txt .c .h .md .cfg .rs …`) / image (`.png`) / other — pick its icon; build the
  `nwui_icon_item` list. Double-click: a directory is entered; a `.nxe` is launched via
  `nwui_spawn`; other files are no-ops.
- **Task pane (left).** Two `nwui_panel`s: **"Other Places"** (links: Home, My Computer, and the
  parent folder) and **"Details"** (the selected item's name + kind, updated from the iconview's
  `on_change`). When at `Location::MyComputer`, Details shows a "System folder / N drives" summary.
- **Navigation chrome.** **Up** and **Home** toolbar buttons + a breadcrumb/path label whose root
  crumb reads "My Computer". Up from a disk mount root returns to the My Computer view (not the raw
  `/disks`). Menus: **File → Close**; **Go → My Computer / Home / Up**.
- **Beauty.** Large icons, generous cell spacing, glass selection accent consistent with the
  compositor.

### D. nwm Start menu

Add **"My Computer"** to `LOGO_ITEMS` (bump `LOGO_NITEMS` to 5). Its handler sets the pending-spawn
slot (`want_spawn` + `run_cmd`) to launch the explorer binary with no path argument (→ My Computer
view). The explorer's `main` opens My Computer when given no path arg, or the given folder when
passed a path.

## Data flow

```
Start menu "My Computer"  ──spawn (want_spawn/run_cmd)──▶  rsexp.nxe (no arg) ──▶ Location::MyComputer
                                                                                  │
  opendir("/disks")  ──▶ drive items ┐                                            │ readdir + classify
  $HOME              ──▶ home item   ┘── nwui_iconview_set ◀─────────────────────-┘
                                          │
  nwui_image_load_png("/nanos/share/icons/<kind>.png")   (libnwui PNG decoder, loaded once at startup)
                                          │
   single-click ──▶ on_change ──▶ update Details panel
   double-click ──▶ enter dir  /  nwui_spawn(.nxe)  /  back to My Computer
```

## Error handling

- `nwui_image_load_png` returns 0 on any failure; the app falls back to the generic `file` icon.
  Icons are pre-loaded once at startup, so a missing icon degrades gracefully (no per-cell I/O,
  no crash).
- `opendir` failure (permissions, missing path) keeps the previous listing, like the current
  `nwexp`.
- A `.nxe` that fails to spawn is a no-op (the compositor reports its own failure); the explorer does
  not block.
- Rust panics abort (`panic-strategy = abort`); the app avoids panics on normal I/O paths (checked
  conversions, no `unwrap` on external data).

## Testing

- **Host tests (toolkit doctest harness, `tests/`):** iconview layout/columns/selection/scroll math;
  `nwui_panel` measure/arrange (title + stacked children); `nwui_image_load_png` decode round-trip
  (decode a committed PNG, assert dimensions + a few pixels). Added to the toolkit host-test build.
- **`make check-arch`:** stays clean — all new code is userland / MI; nothing x86-internal leaks.
- **QEMU screendump (the established headless pattern):** boot `make image64`, open Files from the
  Start menu, confirm My Computer shows the disks + Home with icons and the task pane, navigate into
  `/disks/main`, confirm folder/program/text/image/file icons + the Details panel update, and launch a
  `.nxe`. Capture screendumps at each step.

## Out of scope (YAGNI)

- List/Details *view modes*, columns, sorting UI (the left "Details" *panel* is in; a details *view*
  is not).
- Back/Forward history, editable address bar.
- Cut/copy/paste/rename/delete file operations (this is a browser/launcher, like `nwexp`).
- Icon theming / multiple icon sizes / a settings UI for icons.
- A kernel-level "My Computer" synthetic node.
- Image formats other than PNG; interlaced or >8-bit PNGs (the existing decoder's limits).

## File plan (created / modified)

**Created**
- `user/libnwui/nwui_png.{c,h}` — PNG decoder promoted from nwm + file-load wrapper.
- Icon-grid widget (`NWUI_ICONVIEW`) + titled-panel widget (`NWUI_PANEL`) — code in
  `nwui_core.c`/`nwui_paint.c`, declarations in `nwui.h`/`nwui_core.h`.
- `tools/gen-icons.py` — host icon generator; `assets/icons/*.png` — committed icon set.
- `user/rust/x86_64-nanos.json` — x64 Rust target.
- `user/rust/libnwui-rs/` — reusable safe Rust bindings crate.
- `user/rust/rsexp/` — the Rust explorer app.
- Toolkit host tests for iconview, panel, and the PNG loader.

**Modified**
- `user/nwm/png.{c,h}` — use the libnwui decoder (dedup).
- `user/nwm/nwm_core.c` — add "My Computer" to `LOGO_ITEMS` + spawn handler; bump `LOGO_NITEMS`.
- `Makefile` — x64 Rust app build rule; add explorer to `X64_GUI_APPS`, drop `nwexp`; install
  `/nanos/share/icons/`; toolkit host-test list.

## Risks

- **Rust x86_64 ABI vs libnwui.ndl/libc.ndl** (struct layout, calling convention, soft-float). Mirror
  the proven i686 `rustform` linkage and the x64 `nwexp` link line exactly; validate with a trivial
  Rust `.nxe` (a window + label) before the full app. Fallback: keep the binding surface minimal
  (plain pointers/ints).
- **PNG decoder relocation into libnwui** must not change nwm's wallpaper behavior — covered by a host
  decode round-trip test and a QEMU wallpaper check.
- **Icon-grid + panel** are the largest new C surface — host-test the layout math before wiring the
  Rust app.
