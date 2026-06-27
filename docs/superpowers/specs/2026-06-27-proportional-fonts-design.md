# Proportional TTF Font System + Settings Font Management — Design

**Date:** 2026-06-27
**Status:** Approved (brainstorming → spec)
**Driver:** The rsexp redesign looks modern except the text — NanOS renders a 1-bit 8×16 VGA bitmap
font (`nx_font8x16`, `user/term/vtfont.c`). The user wants real font files (TTF/OTF) with smooth,
**proportional** text (macOS-style) and the ability to **add/select fonts in system Settings**.

## Goal

A real **TrueType/OpenType font engine** in the userland graphics layer that renders **anti-aliased,
proportional** UI text, plus **font management in the Settings app** (pick the active UI font from
fonts under `/nanos/share/fonts/`, persisted and hot-reloaded across the compositor and apps). The
**terminal stays monospace** (its own fixed-cell font). Ship one libre proportional sans (UI) and
one libre monospace (terminal).

## Decisions (locked)

1. **Format:** real **TTF/OTF**, rendered at runtime via vendored **`stb_truetype`** (v1.26, public
   domain — `user/third_party/stb/stb_truetype.h`). No prebaked atlas; glyphs rasterized + cached.
2. **Proportional UI text** (the chosen, ambitious option): variable advances, AA. This requires
   replacing every fixed-width assumption (`strlen * NW_FONT_W`) in the toolkit with real text
   measurement.
3. **Terminal stays monospace:** the VT grid (`user/term/vt.c`, nterm/nwterm) needs fixed cells, so
   it renders a **monospace** font (JetBrains Mono) at a fixed advance — a separate font role.
4. **Two font roles:** `NWFONT_UI` (proportional sans) and `NWFONT_MONO` (monospace). The engine
   loads both; `nw_text*` uses UI, the terminal/`nw_draw_char` uses MONO.
5. **Shipped fonts:** `assets/fonts/UISans-Regular.ttf` (IBM Plex Sans, OFL) + `assets/fonts/
   Mono-Regular.ttf` (JetBrains Mono, OFL), installed to `/nanos/share/fonts/`.
6. **Settings:** the existing settings model (`user/libnw/nw_settings.*`, `settings.yaml`) gains a
   `ui_font` key; nwset lists `*.ttf`/`*.otf` in `/nanos/share/fonts` and lets the user pick the
   active UI font. Change broadcasts a settings reload (the existing `nwui_reload_settings` /
   compositor settings-reload path), so chrome + apps re-render with the new font live.
7. **Adding fonts:** drop a `.ttf`/`.otf` into `/nanos/share/fonts/` and it appears in Settings
   (Font-Book model). Bonus: rsexp gains an "Install font" action when a `.ttf`/`.otf` is opened
   (copies it into the fonts dir). No arbitrary-upload UI beyond the filesystem.

## Architecture

### A. Font engine — `user/libnw/nwfont.{c,h}` (new), over stb_truetype

- `user/libnw/stb_impl.c` — the single translation unit defining `STB_TRUETYPE_IMPLEMENTATION`
  with config macros: `STBTT_malloc/free` → libc, `STBTT_assert` → noop, math via picolibc
  (`floor/ceil/sqrt/pow/fmod/cos/acos/fabs`). Compiled once into libnw.
- `nwfont`: `nwfont_load(role, path)` reads the file into a kept heap buffer, `stbtt_InitFont`,
  computes a scale for a target pixel height. Per-codepoint **glyph cache** (lazy): AA coverage
  bitmap (8-bit) + metrics (`advance`, `bearingX`, `bearingY`, `w`, `h`). API:
  ```c
  enum { NWFONT_UI, NWFONT_MONO };
  int  nwfont_set(int role, const char *path, int px);   /* load/replace a role; 0 on ok */
  int  nwfont_ascent(int role);                          /* baseline offset within the line box */
  int  nwfont_line_h(int role);
  int  nwfont_text_w(int role, const char *s);           /* pixel width of a string */
  const struct nwfont_glyph *nwfont_get(int role, unsigned cp);  /* cached AA glyph + metrics */
  ```
- Codepoints: ASCII + Latin-1 for now (UTF-8 decode is a later enhancement; the toolkit text is
  ASCII today).

### B. `nw_gfx` text path (`user/libnw/nw_gfx.{c,h}`) — proportional + AA

- `nw_text(s, x, y, str, fg)` → render the **UI** font: for each char, fetch the cached glyph, blend
  its coverage over the destination (`nw_blend_pixel(s, x+bx, y+ascent-by, fg, cov)`), advance by the
  glyph advance. **Returns the end x** (already its contract).
- New: `int nw_text_w(const char *s)` → `nwfont_text_w(NWFONT_UI, s)`. Toolkit measures with this.
- `NW_FONT_H` becomes the UI line box height (default 18). The legacy `NW_FONT_W` is **retired from
  UI layout** but kept as the MONO advance for the terminal.
- `nw_draw_char(s, x, y, ch, fg, bg)` (opaque) + the terminal path render the **MONO** font glyph
  (AA, fixed advance) — the terminal keeps a fixed cell. Falls back to the 1-bit `nx_font8x16` only
  if a font isn't loaded (safety).
- If no TTF is loaded yet (engine init failed / pre-load), fall back to the 1-bit VGA font so the
  system never renders blank text.

### C. libnwui — proportional layout (`user/libnwui/`)

Replace every `strlen(text) * NW_FONT_W` with `nw_text_w(text)` and every per-char x with cumulative
advances:
- `nwui_measure`: LABEL/BUTTON/CHECKBOX/LINK widths via `nw_text_w`; heights via `NW_FONT_H`.
- `nwui_paint`: button/label/link/iconview-label centering via `nw_text_w`.
- **Textfield** (`char_at_x`, caret x): map pixel↔caret by walking glyph advances of the buffer.
- **Textarea** (`user/libnwui/nwui_core.c`): word/character wrapping currently uses `cols = w /
  NW_FONT_W`; switch to **pixel-width wrapping** (accumulate advances per line) and caret/hit-testing
  by advances. This is the largest single change.
- `nwui_iconview` label: truncate by pixel width (ellipsis when it overflows the cell).

### D. Settings — `user/libnw/nw_settings.*` + `user/nwset`

- `nw_settings` gains `char ui_font[64]` (basename under `/nanos/share/fonts`) + getter; serialize
  to `settings.yaml`. Default `UISans-Regular.ttf`.
- nwset: a "Fonts" group listing `/nanos/share/fonts/*.{ttf,otf}` (via `nwui_dir_*`) as selectable
  rows; selecting one writes the setting and triggers a reload.
- On reload, the compositor and apps call `nwfont_set(NWFONT_UI, "/nanos/share/fonts/<name>", px)` and
  repaint.

### E. Boot / install

- `_image64` installs `assets/fonts/UISans-Regular.ttf` + `Mono-Regular.ttf` to
  `/nanos/share/fonts/`; creates the dir.
- At startup nwm + apps load UI + MONO from `/nanos/share/fonts` (UI name from settings, default
  UISans). Engine init is best-effort; failure → 1-bit fallback.

## Data flow

```
boot: nwm/app -> nwfont_set(UI, /nanos/share/fonts/<settings.ui_font>, 18)
                 nwfont_set(MONO, /nanos/share/fonts/Mono-Regular.ttf, 16)
draw: nw_text(UI)  -> per-glyph AA coverage (cached) blended at variable advance
      terminal     -> nw_draw_char(MONO) at fixed cell
Settings pick font -> settings.ui_font=<name> -> reload -> nwfont_set(UI,...) -> repaint
add font: copy .ttf into /nanos/share/fonts (shell / rsexp "Install font") -> appears in Settings
```

## Testing

- **Host tests:** `nwfont_text_w` monotonic + matches sum of advances; glyph cache returns stable
  metrics; textarea pixel-wrap produces expected breaks for a known string/width; `nw_text_w`
  vs cumulative advance consistency. (stb_truetype runs natively in the host build with a committed
  test TTF — reuse the shipped fonts as fixtures.)
- **check-arch:** stays clean (all userland/MI).
- **QEMU screendump:** boot, confirm UI text is smooth/proportional (vs the mockup), terminal stays
  monospace + aligned, open Settings → switch font → text changes live. No faults.

## Risks / notes

- **stb_truetype freestanding:** needs malloc + libm; provide STBTT_* config macros. Validate it
  links into the `.ndl` world (libnw) early with a trivial render.
- **Proportional breakage:** the textarea/textfield pixel math is intricate — host-test it before
  wiring the UI. Keep the 1-bit fallback so a font-load failure degrades, not crashes.
- **Per-process font memory:** each process loads the TTFs (~470 KB) + a small glyph cache.
  Acceptable for the few GUI apps; a shared font service is a future optimization.
- **Terminal must not regress:** monospace role + fixed advance; verify column alignment in QEMU.

## Out of scope (YAGNI)

- UTF-8 / complex scripts / shaping / kerning / hinting beyond stb defaults.
- Bold/italic styles, per-app font overrides, font sizes UI (single UI size for now).
- A shared system font daemon (per-process load is fine for now).
- Variable-font axes.
