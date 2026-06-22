# Design: nwnote → a Windows XP-style Notepad (library-first)

Date: 2026-06-22
Status: approved (brainstorm)

## Goal

Turn `user/nwnote` — today a ~90-line append-only clipboard demo with no scrolling,
caret movement, selection, or file I/O — into a real Windows XP-style Notepad.

**Guiding principle (user directive):** whenever a piece of the editor is a *reusable
component*, it goes into the UI toolkit `libnwui` (the "comctl32 of NanWM"), not into
the app. Notepad ends up a thin client like `nwform`/`nwexp`/`nwset`. Every reusable
widget's *pure logic* lands in `nwui_core.c` (host-tested under doctest, **>90% coverage
gated** via `COV_PATTERNS`), its painting in `nwui_paint.c`, its public API in `nwui.h`,
and any I/O in `nwui.c`.

## Platform constraints discovered (drive the design)

- **No multiline editor widget exists.** `nwui_textfield` is single-line, `NWUI_TEXT_CAP
  = 64`, over an *app-owned* buffer (`tbuf`/`tcap`/`tlen`/`caret`/`anchor`). The textarea
  reuses that app-owned-buffer pattern.
- **No Ctrl modifier in the wire protocol.** The compositor (`nwm_core.c` `nw_key`)
  delivers raw scancode + ASCII + a *shift-only* `mods` bit. Super (Cmd) combos are grabbed
  by the compositor (Super+C/X/V → `NW_EV_COPY`/`PASTE`, Super+Q → close). **But Ctrl
  (scancode `0x1D`, right-Ctrl `0x9D`) and the function keys (F3 `0x3D`, F5 `0x3F`) and
  arrows/Home/End/PgUp/Del (extended codes `0xC8/0xD0/0xCB/0xCD/0xC7/0xCF/0xC9/0xD1/0xD3`)
  all arrive verbatim in `nw_event.code`.** So XP-style shortcuts are achievable entirely
  client-side, with **no kernel/compositor changes** — by tracking Ctrl state in the
  toolkit core.
- **The toolkit owns the event loop** (`nwui_run` → `nwui_dispatch`). The app cannot see
  raw keys. Therefore shortcuts must be a toolkit feature: an **accelerator table**.
- **Menus are the global macOS-style bar** (`nwui_menu`/`nwui_menu_item` → `nw_set_menu` →
  `NW_EV_MENU`). Notepad declares File/Edit/Format/View/Help there. (User chose the
  platform global menu bar over an in-window XP menu bar.)
- **Menu items can't take typed input** → Open/Save/Find/Replace/GoTo are **in-window
  modal overlays**, generalising the existing `menu_open` context-menu overlay mechanism.
- **Rendering model:** `nwui_render` does a full repaint on `layout_dirty`, else repaints
  only `dirty` nodes into a damage rect. Overlays (`draw_menu`) are drawn last on full
  repaint. New widgets/overlays follow the same path.

## New reusable `libnwui` components

### 1. `nwui_textarea` — multiline editor (the centerpiece)
- New node kind `NWUI_TEXTAREA`, over an **app-owned** `char*` buffer + cap (like
  textfield). Caret + selection anchor as **byte indices**; `scroll` = top visible row.
- API: `nwui_node *nwui_textarea(nwui*, char *buf, int cap, nwui_cb on_change, void*)`;
  `nwui_textarea_caret(node, int *line, int *col)`; `nwui_textarea_set_wrap(node, int)`;
  `nwui_textarea_find(node, const char *needle, int matchcase, int wrap_around)` (selects
  the match, returns 0/1); `nwui_textarea_goto_line(node, int line1based)`;
  `nwui_textarea_select_all(node)`.
- Pure logic in `nwui_core.c` (host-tested): insert (accepts `\n`/`\t`), delete,
  backspace, delete-range/selection; caret moves L/R/Up/Down/Home/End/PgUp/PgDn with the
  shift-extends-selection rule; a **line index** (offsets of logical line starts) and
  caret↔(row,col)↔(x,y) mapping; **word-wrap** (visual rows from logical lines at the
  widget width); click-to-place-caret and drag-select; scroll-to-keep-caret-visible;
  scrollbar geometry (reuse the list's thumb math style).
- Painting in `nwui_paint.c`: paper bg + border (focus ring), visible rows, selection
  highlight, blinking-less caret bar, vertical scrollbar thumb. Edits/scroll set
  `node->dirty` so only its rect repaints.

### 2. `nwui_checkbox` — labeled toggle
- New kind `NWUI_CHECKBOX`; `int *value` app-owned. `nwui_checkbox(nwui*, const char
  *label, int *value, nwui_cb on_change, void*)`. Click / Space toggles. Pure + painted.
  (Used by Replace's "Match case" and as the Format→Word Wrap reflection if desired.)

### 3. Accelerator table — keyboard shortcuts
- The core tracks `ctrl_down` from scancodes `0x1D`/`0x9D` (press/release). A small table:
  `nwui_accel(nwui*, int ctrl, char key, int fkey, nwui_cb cb, void*)`. On `NW_EV_KEY`
  down, before widget routing, match (ctrl_down, lowercased `ev.ch`) or a function-key
  `ev.code`; if matched, fire the callback and **suppress** the keystroke (so Ctrl+A never
  inserts 'a'). Suppress printable insertion whenever `ctrl_down`. Reuses the *same*
  callbacks the menu items use. Host-tested.
- Clipboard accelerators (Ctrl+C/X/V) route to the focused textarea's copy/cut/paste via
  the existing `clip_set`/`clip_get` hand-off; Super+C/V (`NW_EV_COPY`/`PASTE`) keep
  working too.

### 4. Modal overlay + convenience dialogs
- Generalise the `menu_open` overlay into a **modal sub-tree**: fields on `struct nwui`
  (`modal` root node, `modal_open`, `modal_on_close`, saved focus). `nwui_open_modal(nwui*,
  nwui_node *subtree, nwui_cb on_close)` and `nwui_close_modal(nwui*)`. While open,
  `nwui_dispatch` routes **all** input to the modal subtree only; `nwui_layout` measures it
  and arranges it **centered**; the painter draws a dimmed backdrop then the subtree on top
  (last, like `draw_menu`). Focus moves to the modal's first focusable on open and is
  restored on close.
- Convenience builders on top: `nwui_message(nwui*, title, text)` (info / About + OK) and
  `nwui_prompt(nwui*, title, char *buf, int cap, nwui_cb on_ok, void*)` (label + textfield +
  OK/Cancel). These serve Go To and the unsaved-changes prompt; Find/Replace are composed by
  the app from the modal + primitives (so the library stays minimal).

### 5. `nwui_file_dialog` — Open/Save (built on the modal)
- A modal wrapping a `nwui_list` (directory entries via `getdents64`) + an editable path
  `nwui_textfield` + Open/Save & Cancel buttons; `..` ascends, double-click/Enter opens a
  dir or selects a file. `nwui_file_dialog(nwui*, int save, const char *start_dir, char
  *out_path, int cap, nwui_cb on_ok, void*)`. Pure helpers (path join, `..` normalisation,
  name filtering) host-tested; the `getdents` listing is the I/O part in `nwui.c`.

## What stays app-level in `nwnote.c`

Thin client: owns the text buffer (cap e.g. 64 KiB) and current filename + dirty flag;
builds the window = a `nwui_textarea` (flex) over a status-bar row (`Ln x, Col y`, updated
from `nwui_textarea_caret` via `on_change`); declares the menus; registers accelerators
pointing at the same callbacks; implements the actions:

- **File:** New, Open, Save, Save As (via `nwui_file_dialog` + libc `open/read/write`),
  Exit. Title bar shows the filename; New/Open/Exit honour the dirty prompt.
- **Edit:** Undo (single-level, classic-XP), Cut/Copy/Paste/Delete, Find (Ctrl+F)/Find
  Next (F3)/Replace (Ctrl+H) over `nwui_textarea_find`, Go To (Ctrl+G) via
  `nwui_textarea_goto_line`, Select All (Ctrl+A), Time/Date (F5 — inserts a timestamp).
- **Format:** Word Wrap (toggles `nwui_textarea_set_wrap`). (Font dialog skipped — single
  bitmap font.)
- **View:** Status Bar toggle.
- **Help:** About Notepad (`nwui_message`).

Single-level Undo: snapshot the buffer+caret before each mutating op; Ctrl+Z swaps current
↔ snapshot (faithful to classic Notepad).

## Data flow

`nw_next_event` → `nwui_run` (drains+coalesces) → `nwui_dispatch` (core): accelerators
first (Ctrl/F-keys → app callbacks), else modal routing if a modal is open, else focused
widget (textarea editing / list / textfield) → state change marks nodes dirty / sets
`clip_set`/`clip_get` / `modal_*` → `nwui_render` repaints (full on layout change, else
damage rect) → `nw_commit`. Menu picks arrive as `NW_EV_MENU` → `nwui_menu_dispatch` →
same app callbacks. File I/O and clipboard touch the OS only through `nwui.c`/libc.

## Architecture & build

- `libnwui` gains: `NWUI_TEXTAREA`, `NWUI_CHECKBOX` kinds + their core logic & paint; the
  accelerator table; the modal overlay + `nwui_message`/`nwui_prompt`/`nwui_file_dialog`.
  New constants/fields in `nwui_core.h`; new API in `nwui.h`. Node arena bound
  `NWUI_MAX_NODES` (128) re-checked — bump if the file dialog + modal subtrees need it.
- `nwnote.nxe` re-links against `libnwui` like `nwform` (Makefile line ~1779–1781 changes
  from the libnw-direct link to the `--need libnwui.ndl` chain used by `nwform`/`nwexp`).
  `nwnote.c` is rewritten; no extra app source files needed (logic lives in the toolkit).

## Testing

- **Host doctest** (`tests/test_nwui_core.cpp`, already gated): new TEST_CASEs for the
  textarea (insert/delete/caret moves/line index/word-wrap/find/goto/select-all/click
  mapping/scroll), checkbox toggle, accelerator dispatch + Ctrl-suppression, modal routing
  (input goes to modal, restores focus on close), and the file-dialog pure helpers. Keep
  `nwui_core.c` ≥90% line coverage (the gate).
- **QEMU headless** (per CLAUDE.md): build the image, boot `-display none` with a monitor
  socket, `screendump` → PNG, verify the Notepad window, typing, scrolling, menus, an
  Open/Save round-trip to `/disks/main`, and that the image is e2fsck-clean after a Save.

## Out of scope (YAGNI)

Font/style dialog, Page Setup/Print, multi-level undo, encoding selection, regex find,
right-to-left, an in-window XP-chrome menu bar.
