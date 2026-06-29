---
name: nanos-notepad-libnwui
description: "Notepad app + libnwui toolkit growth (textarea, checkbox, accelerators, modal, file dialog)"
metadata:
  type: project
---

`nwnote` was rebuilt from a ~90-line clipboard demo into a Windows XP-style **Notepad** on
`feat/nwnote-notepad`. Per the user's directive, every reusable piece went into the toolkit
`libnwui` (not the app), so future apps inherit them. Notepad is now a thin client like
nwform/nwexp/nwset (`--need libnwui.ndl`).

**New reusable libnwui widgets** (pure logic in `user/libnwui/nwui_core.c`, host-tested under the
>90% coverage gate; painting in `nwui_paint.c`; opaque public API in `nwui.h`):
- `nwui_textarea` — multiline editor over an app-owned buffer: caret+selection, arrows/Home/End/
  PgUp/PgDn, vertical scroll, optional word-wrap, mouse caret + drag-select, clipboard. Helpers
  `nwui_textarea_caret/_set_wrap/_total_rows/_find/_goto_line/_select_all/_insert_text`.
- `nwui_checkbox`; `nwui_accel` (Ctrl+letter / F-keys — core tracks Ctrl scancode 0x1D itself, NO
  compositor/kernel change; `NWUI_KEY_F3/F5` in the public header); `nwui_focus`.
- Modal overlay (`nwui_open_modal/_close_modal/_modal_open`, centered + input-capturing) +
  `nwui_message`, `nwui_prompt`, `nwui_file_dialog` (the I/O part — opendir/stat — lives in
  `nwui.c`; pure `nwui_path_join`/`nwui_path_up` are in the core). `nwui_post_copy/_post_paste`.

**Gotchas learned:** the app may only use the OPAQUE public API (`nwui.h`) — reaching into node
fields (`->caret`, `->focus`) or `NWUI_SC_*` (internal `nwui_core.h`) breaks the build; expose a
public wrapper instead. While a modal is open, `nwui_render` must force a FULL repaint so it stays
composited on top.

**Build/verify:** canonical target is **x86_64** — `make image64` (i686 `make image` is broken on
this branch: `bootinfo_x86.cpp` returns uint32 vs the uint64 `bootMemTop()` contract; stale 64-bit
objs in bin/ also break the i686 libc link — `make clean` before switching arch). Boot is to a
**bash text console**; type `nwm` to start the desktop, then Super+R → `nwnote`. Verified headless
in QEMU (qemu-system-x86_64, monitor `sendkey` + `screendump`): editor types/renders, status bar
Ln/Col correct, global menu bar shows File/Edit/Format/View/Help, Ctrl+F opens the Find modal.
677 host tests pass, nwui_core.c 96%. Spec/plan: docs/superpowers/{specs,plans}/2026-06-22-nwnote-*.
Related: [[nanos-ui-redesign]].
