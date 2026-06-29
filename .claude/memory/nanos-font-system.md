---
name: nanos-font-system
description: TTF font engine (stb_truetype) + proportional AA UI text + Settings font picker on NanOS GUI
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NanOS GUI got a real **TTF/OTF font engine** (replacing the 1-bit VGA bitmap font). Branch
`feat/rust-explorer` (alongside the rsexp redesign [[nanos-rust-file-explorer]]). Spec/plan:
`docs/superpowers/specs/2026-06-27-proportional-fonts-design.md` + `plans/2026-06-27-proportional-fonts.md`.

- **Engine**: `user/libnw/nwfont.{c,h}` over vendored `stb_truetype` (public domain,
  `user/third_party/stb/`, impl unit `user/libnw/stb_impl.c`). Two roles: `NWFONT_UI`
  (proportional) + `NWFONT_MONO` (fixed). Lazy 256-glyph AA cache; picolibc libm resolves stb math.
- **Rendering** (`user/libnw/nw_gfx.c`): `nw_text` renders the proportional AA UI font (baseline =
  y+ascent, blend coverage via nw_blend_pixel); `nw_text_w` measures; `nw_draw_char_t` renders the
  MONO font (AA) in the fixed NW_FONT_W cell for text inputs (caret/selection grid math unchanged).
  **1-bit VGA fallback** kept if a TTF is missing — text never goes blank. The **terminal stays on
  VGA mono** (nw_draw_char untouched) — aligned.
- **libnwui**: measure/paint use `nw_text_w` (label/button/link/checkbox + iconview truncate/center).
- **Fonts shipped** (OFL): IBM Plex Sans (`UISans-Regular.ttf`, UI) + JetBrains Mono
  (`Mono-Regular.ttf`, inputs) → installed to `/nanos/share/fonts/` by `_image64`. Add a font = drop
  a `.ttf`/`.otf` there.
- **Settings**: `nw_settings` gained `ui_font` key (default UISans). `nw_gfx` loads the configured
  font from settings at process start (`load_ui_from_settings`); `nw_font_reload_from_settings()`
  switches it live (nwm calls it on want_reload). nwset has a "UI font" row that cycles the fonts dir.
- QEMU-verified: smooth proportional UI; injecting `ui_font: Mono-Regular.ttf` re-renders the whole
  UI in JetBrains Mono. check-arch clean, host suite green.

**Deferred** (noted, not done): fully **proportional text EDITING** (textarea pixel-wrap — big editor
rewrite; inputs are smooth-mono for now); rsexp "Install font" action (Phase 5); per-app live font
reload without relaunch (apps pick up a font change on next launch; chrome updates live).
Why proportional inputs were deferred: the textarea wrap/caret is deeply char-column based and
nwnote isn't autostarted — mono-AA was the low-risk correct increment. See [[no-shortcuts-on-foundations]].
