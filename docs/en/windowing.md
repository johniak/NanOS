# NanWM — the NanOS Windowing System

NanWM is the NanOS GUI stack: a userland **compositor** (`nwm`) that owns the framebuffer and the
input devices, a client library (`libnw.ndl`) apps use to open windows and draw, and a widget
toolkit (`libnwui.ndl`) on top. Everything is a normal ring-3 `.nxe` process talking over pipes —
there is no GUI code in the kernel beyond the `/dev/fb0` framebuffer and the `/dev/input*` evdev
nodes.

The split mirrors a classic OS:

| NanWM | Windows analogue | What it is |
|---|---|---|
| `nwm` | the display server / DWM | owns the screen + input, composites windows |
| `libnw.ndl` | `user32` + `gdi32` | connect, create window, draw pixels, pump events |
| `libnwui.ndl` | `comctl32` / Qt | composable widgets, layout, event dispatch |

```
/dev/fb0 (mmap) ─┐                         ┌─ app (e.g. notepad)  →  libnw  ──┐
/dev/input0 (kbd)┤   nwm  (compositor)     │  app (e.g. form)  →  libnwui →┤  libnw
/dev/input1 (mouse)─►  poll → composite ───┤      pipe fds 3 (req) / 4 (evt) │
                       → blit to /dev/fb0   └─ app (rustform)    →  libnwui ──┘ (Rust via C FFI)
```

The pixel format throughout is **32-bpp BGRX** (`0x00RRGGBB` little-endian). For the `.ndl`
mechanism see nxe-ndl.md; for the `/dev/fb0` + `/dev/input*` device side see
filesystem.md (and the PS/2 mouse driver is a kext, kext.md).

---

## 1. The compositor `nwm`

`user/nwm/` — a single userland `.nxe` (`/nanos/bin/nwm`), launched by running `nwm` from the shell.
It immediately spawns the demo desktop (Terminal, Settings, Files) and runs until the last GUI
client exits, then restores the text console. Code is MI/MD-split for host testing:

- **`nwm.c`** — the I/O shell: the only part that touches hardware (fb + input + client pipes).
- **`nwm_core.c`** — pure compositor state: window list, z-order, hit-test, input decode, damage,
  the client output rings. No I/O — host-tested.
- **`nw_compose.c`** — scene rendering + compositing (wallpaper, decorations, blur/glass blend).

### Owning the screen and input

At start `nwm` opens `/dev/fb0`, queries geometry with `ioctl(FBIOGET_VSCREENINFO/FSCREENINFO)`, and
`mmap`s the framebuffer `MAP_SHARED`; it opens `/dev/input0` (keyboard scancode pairs) and
`/dev/input1` (mouse evdev `input_event`s). While `nwm` runs it draws pixels straight into the
mapped framebuffer, taking over from the kernel fbcon text console; on exit the console resumes.

### The poll loop (`nwm.c`)

A single `poll(-1)` blocks on all inputs and every client's request/event pipe, then each wake:

1. **Coalesce** — drain *all* pending keyboard, mouse, and client-request bytes into state
   (`nw_key`, `nw_pointer`, `nw_client_msg` in `nwm_core`) before drawing once. Mouse REL/BTN events
   accumulate and fire on `EV_SYN`.
2. **Reconcile** — allocate content + frame-cache buffers for newly created windows.
3. **Present** — if the scene is dirty: re-render only dirty *window frames*, recompose only the
   damaged screen region, `memcpy` that rect to the framebuffer, then overlay the cursor.
4. **Flush events** — drain each client's output ring to its event pipe (drop a client whose ring
   overflows).

### Damage tracking + per-window frame cache (the perf core)

The compositor never repaints the whole screen if it can avoid it:

- **Scene damage** is a single union bounding box; `present()` recomposes only that rect and blits
  only that rect.
- **Per-window frame cache**: a window's *chrome+content* is rendered once into a window-local cache
  buffer; compositing reads from the cache. A COMMIT, a CREATE, or a focus change sets the window's
  `frame_dirty` (the titlebar dims on focus loss) — but **moving a window (x/y change) does NOT**,
  so a drag is just *wallpaper memcpy + composite-from-cache + blit*, with zero chrome re-render.
  This (plus the rasterizer fast paths in §3) gave the ~10× drag speedup recorded in
  `../superpowers/plans/2026-06-11-gui-performance.md`.

### Compositing & the glass look (`nw_compose.c`)

The base layer is a cached procedural **wallpaper** (radial-gradient blobs over a diagonal
gradient). Each window composites over it with **rounded, anti-aliased corners** and a per-window
**alpha blend** (≈234/236) for the translucent "glass" material; a translucent **top menu bar** and
**dock** and an outlined **cursor** are drawn last. The blend uses the **RB-paired `nw_blend8`** (red
and blue multiplied together, green separately → 2 multiplies/pixel instead of 6), with `a`
normalized to 0..256 so fully-opaque and fully-transparent are exact. The compositor draws all
window **decorations** (gradient titlebar, close/min boxes) — clients only supply content pixels.

---

## 2. The client↔compositor protocol (`user/libnw/nwproto.h`)

**Transport.** A byte stream over an inherited **pipe pair**: the client writes requests on **fd 3**
and reads events on **fd 4**. `nwm` creates the two pipes, `dup2`s them to 3/4, and `exec`s the
client (`nwm.c:132`), so a client just uses fds 3/4 — no socket, no name lookup.

**Framing.** A fixed **28-byte header** `nw_msg { type, window, a, b, c, d, length }` (all 4-byte,
`static_assert`ed to 28) optionally followed by `length` payload bytes. Because the kernel pipe ring
is only 4096 B, a window-pixel COMMIT arrives in many fragments, so both sides decode with a
**streaming state machine** (`nw_decoder`) that reassembles header-then-payload across reads and
never blocks on a partial message.

**Buffer sharing is copy, not shared memory.** A client renders into *its own* window pixel buffer
and sends damaged rectangles as COMMIT messages; the compositor reassembles them into the window's
content buffer. A repaint larger than `NW_COMMIT_MAX_BYTES` (256 KB) is split into horizontal row
bands so it fits the compositor's per-client reassembly buffer.

| Client → server (`NW_REQ_*`) | Args |
|---|---|
| `HELLO` (1) | version handshake |
| `CREATE_WINDOW` (2) | `a=w b=h`, payload = title |
| `COMMIT` (3) | `window`, `a=x b=y c=w d=h`, payload = `c*d*4` BGRX pixels |
| `DESTROY_WINDOW` (4) | `window` |
| `SET_CLIPBOARD` (5) / `GET_CLIPBOARD` (6) | clipboard text in/out |
| `SPAWN` (7) | payload = command to launch (the Run path) |
| `SET_MENU` (8) | payload = app-menu spec (menus split `0x1e`, fields `0x1f`) |

| Server → client (`NW_EVT_*`) | Args |
|---|---|
| `CONFIGURE` (64) | `a=w b=h` assigned size (incl. first map) |
| `KEY` (65) | `a=ascii b=down c=scancode d=mods` |
| `POINTER` (66) | `a=x b=y` (window-relative) `c=buttons` |
| `FOCUS` (67) | `a=1/0` |
| `CLOSE` (68) | user asked to close |
| `COPY` (69) / `PASTE` (70) | Super+C/X → reply SET_CLIPBOARD; Super+V → payload = text |
| `MENU` (71) | `a=top-menu b=item` chosen from the menu bar |

---

## 3. `libnw.ndl` — the client library (`user/libnw/`)

The "user32/gdi32" of NanWM (`libnw.h`), imported by name. A minimal app:

```c
nw_display *d   = nw_connect();                          /* fds 3/4 */
nw_win     *win = nw_create_window(d, 400, 300, "Hello");/* blocks for CONFIGURE */
struct nw_surface s; nw_win_surface(win, &s);            /* the window's pixel buffer */
nw_fill_rect(&s, 0, 0, s.w, s.h, 0x202830);
nw_text(&s, 16, 16, "hello from NanWM", 0xE0E0E0);
nw_commit(win, 0, 0, s.w, s.h);                          /* push damage */
struct nw_event ev;
while (nw_next_event(d, &ev, -1) == 1) {                 /* GetMessage/DispatchMessage */
    if (ev.type == NW_EV_CLOSE) break;
    /* NW_EV_KEY / NW_EV_POINTER / NW_EV_FOCUS / NW_EV_PASTE / NW_EV_MENU … */
}
```

Other entry points: `nw_set_clipboard`/`nw_get_clipboard`, `nw_spawn` (launch a program),
`nw_set_menu` (declare the app menu), and `nw_event_fd` (so a client like the terminal can `poll`
the compositor alongside its own fds, e.g. a pty master, then drain with `nw_next_event(d, ev, 0)`).

### `nw_gfx` — the rasterizer (`nw_gfx.h/c`)

Pure, fully-clipped software drawing over a `nw_surface { px, w, h, stride, clip_* }` — the same
code the compositor uses for decorations and clients use for content. Font is the **public-domain
8×16 IBM VGA bitmap** (`nx_font8x16`, shared with the `nterm` terminal). Primitives: `nw_put_pixel`,
`nw_fill_rect`, `nw_blit`, `nw_draw_char`/`nw_text`/`nw_draw_text`, and compositing helpers
`nw_blend_rect` (alpha), `nw_vgrad_rect` (gradient), `nw_fill_round`/`nw_stroke_round` (AA rounded
rects). An optional per-surface scissor (`nw_surface_clip`) is how the compositor recomposes only
the damaged region. Hot paths use per-row `memcpy` blits and the RB-paired blend.

---

## 4. `libnwui.ndl` — the widget toolkit (`user/libnwui/`)

The "comctl32/Qt" (`nwui.h`): client-side widgets that render into the app's libnw window buffer —
the compositor stays a dumb pixel server. The model is a **composable node tree** (containers nest
children), a **flex-lite layout** engine (bottom-up measure, top-down arrange, main-axis weights),
and **unidirectional data flow** (event → callback → state change → repaint), with per-node damage
feeding libnw's damage path. Split MI/MD again: `nwui_core.c` (tree, layout, hit-test, event
routing — host-tested), `nwui_paint.c` (rendering), `nwui.c` (I/O shell).

The ABI is **plain C** (opaque handles, POD, function-pointer callbacks) on purpose, so any language
with C FFI can build NanWM apps — `rustform` is the same demo written in Rust.

```c
nwui *u = nwui_open("Form", 360, 200);                 /* does connect + create_window */
nwui_node *root = nwui_column(u,
    nwui_label(u, "Name:"),
    nwui_textfield(u, namebuf, sizeof namebuf, on_change, 0),
    nwui_button(u, "Greet", on_click, u),
    (nwui_node*)0);
nwui_set_root(u, nwui_pad(root, 12));
nwui_run(u);                                            /* event loop until closed */
```

- **Widgets**: `nwui_label`, `nwui_button`, `nwui_textfield` (over an app-owned buffer),
  `nwui_list` (+ `nwui_list_set`/`nwui_list_selected`, single-click select, double-click/Enter
  activate, scrollbar), `nwui_image`, `nwui_checkbox` (app-owned flag), and
  `nwui_textarea` — a full multiline editor over an app-owned buffer (caret + selection,
  arrow/Home/End/PgUp/PgDn, vertical scroll, optional word-wrap, mouse caret + drag-select,
  clipboard). Helpers: `nwui_textarea_caret` (line/col), `_set_wrap`, `_total_rows`, `_find`,
  `_goto_line`, `_select_all`, `_insert_text`.
- **Containers**: variadic `nwui_column`/`nwui_row`/`nwui_box`; FFI-friendly `nwui_vbox`/`nwui_hbox`
  + `nwui_add`.
- **Layout props** (chainable): `nwui_pad`, `nwui_gap`, `nwui_flex`, `nwui_size`, `nwui_colors`.
- **State**: `nwui_set_text`/`nwui_get_text` mark the node dirty → repaint; `nwui_focus` sets the
  focused widget.
- **App menu**: `nwui_menu` / `nwui_menu_item` / `nwui_menu_separator` populate the global
  macOS-style menu bar (the first menu's title is the app name); a pick fires the item callback.
- **Keyboard accelerators**: `nwui_accel(u, cmd, key, fkey, cb, user)` — **Cmd+&lt;letter&gt;** (the
  macOS-style ⌘/Super key; `cmd=1`) or a function key (`NWUI_KEY_F3`/`F5`). The compositor forwards
  Cmd+&lt;key&gt; to the focused window with the **Cmd mod bit** (`mods` bit1) set; the toolkit matches
  on it and accelerators share the menu callbacks. See §5.1 — **NanOS uses Cmd for ALL shortcuts**.
- **Iconview clipboard**: `nwui_iconview_set_clipboard(n, on_copy, on_paste)` lets a grid (e.g. a
  file manager) handle **Cmd+C / Cmd+X / Cmd+V** as *file* copy/cut/paste — the same shortcuts that
  copy/paste *text* in a focused field — because the compositor delivers them as COPY/PASTE events
  to whatever widget is focused (`nwui_iconview_copy_cut` tells copy vs cut).
- **Drag and drop** (between windows): the compositor arbitrates it (only it knows geometry/z-order/
  cursor). `nwui_iconview_set_dnd(n, on_drag, on_drop)` — a press that moves past a threshold fires
  `on_drag` (call `nwui_begin_drag(u, payload)`); a drop fires `on_drop` (read `nwui_iconview_drop_cell`
  + `_drop_text` + `_drop_mods`). The grid highlights the hovered drop-target cell.
- **Textfield extras**: `nwui_textfield_set` (set the value), `nwui_textfield_set_submit` (Enter
  callback, vs per-keystroke `on_change`), `nwui_textfield_select_all` (e.g. on Cmd+L address-bar).
- **Modal dialogs**: `nwui_open_modal`/`nwui_close_modal`/`nwui_modal_open` show a centered,
  input-capturing sub-tree over a dimmed backdrop. Built on it: `nwui_message` (alert/About),
  `nwui_prompt` (label + field + OK/Cancel), and `nwui_file_dialog` (Open/Save — a directory list +
  editable path field; pure path helpers `nwui_path_join`/`nwui_path_up`). `nwui_post_copy`/
  `nwui_post_paste` drive the focused field's clipboard from a menu item.
- **`nwui_spawn`** launches another program (e.g. the file manager opening an app).
- **`nwui_open_file(u, path)`** — macOS-style **open**: launch a file in its associated app (a
  `.nxe` runs; other files open in the app mapped to their extension in
  `/disks/main/nanos/config/associations.conf`, edited in Settings → Default Apps). The resolver
  lives in the shared `user/open/launch.h`.

### 5.2 `open` from a terminal (the launch socket)

The **same** open works from the command line. The compositor listens on an AF_UNIX **launch
socket** (`/tmp/.nwm-spawn`): a non-window process connects and sends `"cmd\0arg"`, and nwm spawns
the app as a new window client (resolved like the Run dialog). The **`open`** command
(`/nanos/bin/open`) resolves a file's associated app (the same `launch.h` logic the UI uses) and
asks nwm over that socket — so `open report.txt` / `open photo.png` from a Terminal running in the
desktop launches the right GUI app, exactly like macOS `open`. (A terminal must be *inside* the
nwm session — a non-window process can't draw, only ask the desktop to launch.)

---

## 5. Input routing

```
/dev/input0 (kbd scancodes) ─┐                      ┌─ hit-test (nw_hit): CONTENT/TITLE/CLOSE/MIN
/dev/input1 (mouse evdev) ───►  nwm: decode + coalesce │  focus (click-to-focus, Super+Tab cycle)
                                (scancode→ASCII US,     │  drag (grab titlebar → move window)
                                 mouse on EV_SYN)       └─ emit to the target client's event ring
                                                            → libnw event queue → app / widget
```

The compositor decodes keyboard scancodes to ASCII (US layout, modifiers) and coalesces mouse
deltas/buttons, then **hit-tests** the topmost window at the cursor and classifies the region
(content, titlebar, close/min box). Titlebar press starts a window **drag**; a click **raises +
focuses** the window (Super+Tab cycles focus). Events for the focused/hit window are queued on that
client's output ring and delivered as `NW_EVT_*`.

### 5.1 Keyboard shortcuts are macOS-style **Cmd (⌘)** — everywhere

NanOS uses the **Cmd key (the ⌘/Super/"GUI" key, `meta` in QEMU) for ALL keyboard shortcuts**, like
macOS — never Ctrl. There are two layers, both keyed on Cmd:

- **System (compositor-owned) shortcuts** — handled by `nwm`, work in any app:
  - **Cmd+C / Cmd+X / Cmd+V** — copy / cut / paste (delivered to the focused window as `COPY`/`PASTE`
    events; a text field copies text, a file grid copies files — same keys, context-sensitive).
  - **Cmd+Q** close window · **Cmd+Tab** cycle windows · **Cmd+M** maximize/restore · **Cmd+R** Run.
- **App (per-window) shortcuts** — every *other* Cmd+&lt;key&gt; is forwarded to the focused window with
  the **Cmd mod bit** (`NW_EVT_KEY` `mods` bit1); the app matches it via `nwui_accel(u, cmd=1, …)`.
  Examples: Files — **Cmd+N** New Folder, **Cmd+L** focus the address bar; Notepad — **Cmd+S** Save,
  **Cmd+O** Open, **Cmd+F** Find, **Cmd+A** Select All, **Cmd+Z** Undo. Function keys (F2 rename,
  F5 refresh, F3 find-next) are modifier-less.

Rule of thumb when writing an app: register shortcuts with `nwui_accel(u, /*cmd=*/1, key, …)`; do NOT
register Cmd+C/X/V (the compositor owns them — handle the COPY/PASTE events instead). The compositor
decodes scancodes to ASCII (US layout) and tracks Shift (mods bit0) and Cmd (mods bit1).

---

## 6. The framebuffer (`/dev/fb0`)

`nwm` is an ordinary fbdev client: `FBIOGET_VSCREENINFO`/`FSCREENINFO` for geometry, `mmap` of the
linear framebuffer, 32-bpp BGRX with a pixel stride of `line_length/4`. It keeps a screen-sized
**`g_scene`** (the composed frame), a cached **`g_wall`** (wallpaper), and a **`g_scratch`** work
buffer; only the per-frame damage rect is `memcpy`'d from `g_scene` into the mapped framebuffer, so a
small change costs a small blit. (The kernel framebuffer/fbcon side is `Fb0Device`/`Framebuffer.*`;
see filesystem.md and the graphics stack.)

---

## 7. Applications & build

| App | Library | What it is |
|---|---|---|
| `terminal` | libnwui | terminal emulator in a window (polls its pty master via `nw_event_fd`) |
| `settings` | libnwui | system settings |
| `nwexp` | libnwui | file explorer (list widget, directory nav) |
| `form` | libnwui | greeting form (textfield + buttons) — the toolkit demo |
| `about` | libnwui | about dialog |
| `notepad` | libnwui | **Notepad** — a Windows XP-style editor (menus, accelerators, find/replace/go-to, open/save, status bar) on the `nwui_textarea` + modal widgets |
| `rustform` | libnwui via **Rust** FFI | the same form in Rust — proves the C ABI is language-agnostic |

At boot the compositor spawns `terminal`, `settings`, `nwexp` (Files last → on top + focused).

**Build (Makefile).** `libnw.ndl` and `libnwui.ndl` are built like any shared library (→
`/nanos/lib`), the shared cores `nwproto.o`/`nw_gfx.o` link into both `nwm` and the clients. `nwm` is
a `SYS_PROG` (→ `/nanos/bin`); the demo apps are `APP_PROGS` (→ `/apps/<name>/` bundles, reached via
the `/bin` link farm). `rustform` is a `cargo` `no_std` staticlib for a custom `i686-nanos.json`
target (`-Z build-std=core,alloc`), linked through crt0 + `libnwui.ndl.a` + `libc.ndl.a`.

**Testing.** The pure cores are in the coverage gate (`TEST_MODULES` / `COV_PATTERNS`):
`nwm_core`, `nw_compose`, `nwproto`, `nw_gfx`, `nwui_core` all run host-side under doctest — the
protocol decoder, layout, hit-test, damage union and compositing math are tested without a
framebuffer.

---

## 8. See also

- `../superpowers/plans/2026-06-11-gui-performance.md` — the drag-performance diagnosis and the
  raster/blend/frame-cache optimizations (≈10× frame-time win).
- `.claude/nanoos-ui/` — the static HTML mockup of the desktop concept (dock, menu bar, glass
  windows).
- nxe-ndl.md (the `.ndl` libraries), kext.md (the PS/2 mouse kext feeding
  `/dev/input1`), filesystem.md (`/dev/fb0`, `/dev/input*`).
