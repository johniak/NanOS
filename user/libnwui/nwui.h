/*
 * nwui.h — NanWM UI toolkit (the "comctl32" of NanWM): a modern, composable widget library.
 *
 * Client-side (like Qt/GTK/Flutter): widgets render into the app's window buffer and route
 * events; the compositor stays a dumb pixel server. The model is a COMPOSABLE TREE (containers
 * nest children, a flex-lite layout engine sizes them) with UNIDIRECTIONAL data flow (event ->
 * callback -> state change -> repaint) and per-node damage that feeds the compositor's existing
 * damage path.
 *
 * The ABI is plain C (opaque handles, POD, function-pointer callbacks) on purpose, so any
 * language with C FFI — including Rust — can build NanWM apps, exactly like Windows apps bind
 * to user32. Shipped as the shared library libnwui.ndl.
 */
#ifndef NWUI_H
#define NWUI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nwui      nwui;        /* a toolkit instance bound to one window */
typedef struct nwui_node nwui_node;   /* a widget / container in the tree       */

/* Function-key codes for nwui_accel's `fkey` argument (normalized scancodes). */
enum { NWUI_KEY_F3 = 0x3D, NWUI_KEY_F5 = 0x3F };

/* A widget callback: the firing node + the user pointer it was created with. */
typedef void (*nwui_cb)(nwui_node *self, void *user);

/* ---- lifecycle ---- */
/* Open a window and bind a toolkit to it (does the nw_connect + nw_create_window for you, so
 * the app never touches libnw directly). NULL on failure. */
nwui *nwui_open(const char *title, int w, int h);
/* Install the root of the widget tree (built with the component functions below). */
void  nwui_set_root(nwui *u, nwui_node *root);
/* Give keyboard focus to a widget (e.g. focus the editor at startup). */
void  nwui_focus(nwui *u, nwui_node *n);
/* Run the event loop: pump input, dispatch to widgets, repaint damaged nodes — until the
 * window is closed. */
void  nwui_run(nwui *u);

/* ---- components (allocated from the toolkit's node arena; do not free) ---- */
nwui_node *nwui_label(nwui *u, const char *text);
nwui_node *nwui_button(nwui *u, const char *text, nwui_cb on_click, void *user);
/* A w*h image (px = app-owned 0x00RRGGBB buffer) painted at its natural size. */
nwui_node *nwui_image(nwui *u, const uint32_t *px, int w, int h);
/* Load a PNG file into a malloc'd w*h 0x00RRGGBB buffer (caller frees). 0 on any failure. */
uint32_t *nwui_image_load_png(const char *path, int *w, int *h);
/* An editable field over an APP-OWNED buffer (the app reads the typed value straight from it). */
nwui_node *nwui_textfield(nwui *u, char *buf, int cap, nwui_cb on_change, void *user);
/* Set a textfield's displayed value programmatically (no on_change fired). */
void       nwui_textfield_set(nwui_node *n, const char *s);
/* Fire `cb` when Enter is pressed in the textfield (distinct from per-keystroke on_change). */
void       nwui_textfield_set_submit(nwui_node *n, nwui_cb cb);
/* Select the whole field so the next keystroke replaces it (e.g. focusing an address bar). */
void       nwui_textfield_select_all(nwui_node *n);

/* A multiline text editor over an APP-OWNED buffer: caret + selection + scroll + optional
 * word-wrap. The reusable heart of any text app (Notepad, log viewer, code box). */
nwui_node *nwui_textarea(nwui *u, char *buf, int cap, nwui_cb on_change, void *user);
/* Report the caret position as 1-based line + column (for a status bar). */
void       nwui_textarea_caret(nwui_node *n, int *line, int *col);
/* Turn word-wrap on/off (off = one visual row per logical line). */
void       nwui_textarea_set_wrap(nwui_node *n, int on);
/* Total visual rows under the current wrap setting (for scrollbar sizing). */
int        nwui_textarea_total_rows(nwui_node *n);
/* Search from the caret; on a hit select the match + scroll to it, return 1 (else 0). */
int        nwui_textarea_find(nwui_node *n, const char *needle, int matchcase, int wrap_around);
/* Move the caret to the start of 1-based line `line1` (clamped) + scroll to it. */
void       nwui_textarea_goto_line(nwui_node *n, int line1);
/* Select the whole buffer. */
void       nwui_textarea_select_all(nwui_node *n);
/* Insert a C string at the caret. */
void       nwui_textarea_insert_text(nwui_node *n, const char *s);

/* Programmatic clipboard for the focused field (so a menu item can drive it): copy/cut the
 * selection to the clipboard; paste asks the compositor (reply arrives as a paste event). */
void       nwui_post_copy(nwui *u, int cut);
void       nwui_post_paste(nwui *u);

/* ---- keyboard accelerators (menu shortcuts) ----
 * NanOS uses macOS-style Cmd (the Super/⌘ key) for ALL shortcuts. Register Cmd+<letter>
 * (cmd=1, key='s') or a function key (key=0, fkey=NWUI_KEY_F3). On a match the callback fires
 * and the keystroke is consumed; share the menu callbacks. The compositor owns the system-wide
 * Cmd shortcuts (Cmd+C/X/V clipboard, Cmd+Q quit, Cmd+Tab, Cmd+M, Cmd+R run) and forwards every
 * other Cmd+<key> to the focused window for the app to match here. */
void       nwui_accel(nwui *u, int cmd, char key, int fkey, nwui_cb cb, void *user);

/* ---- modal overlay ----
 * Show `subtree` centered over the window; it captures ALL input until dismissed. Focus moves
 * to its first focusable child; closing restores the prior focus and fires on_close (if set). */
void       nwui_open_modal(nwui *u, nwui_node *subtree, nwui_cb on_close, void *user);
void       nwui_close_modal(nwui *u);
int        nwui_modal_open(const nwui *u);

/* Info dialog: a title, a body line, an OK button that closes the modal (About boxes, alerts). */
void       nwui_message(nwui *u, const char *title, const char *text);
/* Input dialog over an APP-OWNED buffer: title + textfield + OK/Cancel. OK fires on_ok then
 * closes; Cancel just closes. (Find, Go To, rename, …) */
void       nwui_prompt(nwui *u, const char *title, char *buf, int cap, nwui_cb on_ok, void *user);
/* Confirmation dialog: title + body + an affirmative button (label `ok_label`) that fires on_yes
 * then closes, plus a Cancel button. (Delete confirmation, overwrite prompts, …) */
void       nwui_confirm(nwui *u, const char *title, const char *text, const char *ok_label,
                        nwui_cb on_yes, void *user);

/* ---- path helpers (pure) + the file open/save dialog ---- */
void       nwui_path_join(const char *dir, const char *name, char *out, int cap);
void       nwui_path_up(char *path);
/* A modal file chooser: a directory listing + an editable path field (bound to out_path) +
 * Open/Save & Cancel. save=1 = "Save As". OK fires on_ok with out_path set. */
void       nwui_file_dialog(nwui *u, int save, const char *start_dir,
                            char *out_path, int cap, nwui_cb on_ok, void *user);

/* A labeled toggle over an APP-OWNED int flag (0/1). Click or Space flips it. */
nwui_node *nwui_checkbox(nwui *u, const char *label, int *value, nwui_cb on_change, void *user);

/* A scrollable list of rows. Items are an APP-OWNED array of strings (set with nwui_list_set).
 * A single click selects a row; a DOUBLE-click (or Enter) fires on_activate ("open"); arrows
 * move the selection and a scrollbar appears when rows overflow. The app reads which row fired
 * with nwui_list_selected. */
nwui_node *nwui_list(nwui *u, nwui_cb on_activate, void *user);
void       nwui_list_set(nwui_node *list, const char *const *items, int count);
int        nwui_list_selected(nwui_node *list);

/* One icon-grid cell: a label plus an icon (app-owned w*h 0x00RRGGBB buffer; may be shared across
 * cells). Authored icons are 48x48; the view blits them at natural size, centered in the cell. */
typedef struct { const char *label; const uint32_t *icon; int iw, ih; } nwui_icon_item;

/* A wrapping grid of large icons. A single click selects a cell (fires on_change); a DOUBLE-click
 * (or Enter) fires on_activate ("open"). Arrows move the selection and auto-scroll. Items are an
 * APP-OWNED array (set with nwui_iconview_set); read the selection with nwui_iconview_selected. */
nwui_node *nwui_iconview(nwui *u, nwui_cb on_activate, nwui_cb on_change, void *user);
void       nwui_iconview_set(nwui_node *n, const nwui_icon_item *items, int count);
int        nwui_iconview_selected(nwui_node *n);

/* ---- drag-and-drop (between windows, arbitrated by the compositor) ----
 * Make an iconview a drag source + drop target. `on_drag` fires once when a press on a cell
 * turns into a drag — respond by calling nwui_begin_drag(u, payload). `on_drop` fires when a
 * drop lands on the view — read nwui_iconview_drop_cell (the cell under the drop, or -1 for the
 * empty area) and nwui_iconview_drop_text (the payload, valid only during the callback). */
void        nwui_iconview_set_dnd(nwui_node *n, nwui_cb on_drag, nwui_cb on_drop);
/* Make an iconview handle the clipboard shortcuts: on_copy (Cmd+C / Cmd+X — read
 * nwui_iconview_copy_cut for which) and on_paste (Cmd+V). Lets a file manager do file
 * copy/cut/paste with the same macOS-style Cmd shortcuts as text. */
void        nwui_iconview_set_clipboard(nwui_node *n, nwui_cb on_copy, nwui_cb on_paste);
int         nwui_iconview_copy_cut(nwui_node *n);   /* during on_copy: 1 = cut (Cmd+X), 0 = copy */
int         nwui_iconview_drop_cell(nwui_node *n);
int         nwui_iconview_drop_mods(nwui_node *n);   /* modifier bits at the drop (bit1 = ctrl) */
const char *nwui_iconview_drop_text(nwui_node *n);
/* Start a drag carrying `text` as the payload (call from an on_drag handler). */
void        nwui_begin_drag(nwui *u, const char *text);

/* A titled panel (glass group-box) — the building block of a sidebar/task pane. Add children with
 * nwui_add(); they stack vertically beneath the title header. */
nwui_node *nwui_panel(nwui *u, const char *title);

/* A flat sidebar link/nav-row: no button gradient, click fires on_click. Mark the row for the
 * CURRENT location with nwui_link_set_active(n, 1) and it renders as a filled accent pill. The
 * macOS-style sidebar is a column of these under small section-header labels. */
nwui_node *nwui_link(nwui *u, const char *text, nwui_cb on_click, void *user);
void       nwui_link_set_active(nwui_node *n, int active);

/* A flat clickable icon button (toolbar). icon = app-owned iw*ih 0xAARRGGBB buffer (alpha-blended,
 * centered); click fires on_click. */
nwui_node *nwui_iconbtn(nwui *u, const uint32_t *icon, int iw, int ih, nwui_cb on_click, void *user);

/* ---- containers: variadic, NULL-terminated children (this is the nesting) ---- */
nwui_node *nwui_column(nwui *u, ...);   /* nwui_column(u, a, b, c, (nwui_node*)0)  */
nwui_node *nwui_row(nwui *u, ...);
nwui_node *nwui_box(nwui *u, nwui_node *child);

/* Non-variadic container building (FFI-friendly for Rust/other languages): make an empty
 * column/row, then append children. nwui_add returns the parent so calls chain. */
nwui_node *nwui_vbox(nwui *u);                              /* empty column */
nwui_node *nwui_hbox(nwui *u);                              /* empty row    */
nwui_node *nwui_add(nwui_node *parent, nwui_node *child);

/* ---- layout props (setters return the node, so they chain) ---- */
nwui_node *nwui_pad(nwui_node *n, int pad);
nwui_node *nwui_gap(nwui_node *n, int gap);
nwui_node *nwui_flex(nwui_node *n, int weight);
nwui_node *nwui_size(nwui_node *n, int w, int h);
nwui_node *nwui_colors(nwui_node *n, uint32_t fg, uint32_t bg);

/* ---- state mutation (marks the node dirty -> repaint) ---- */
void        nwui_set_text(nwui_node *n, const char *text);
const char *nwui_get_text(nwui_node *n);

/* Ask the compositor to launch a program (by name or absolute path), the same way the Super+R
 * Run dialog does — used e.g. by a file manager to open/run an app. Fire-and-forget. */
void        nwui_spawn(nwui *u, const char *cmd);

/* Launch `cmd` passing `arg` as its argv[1] (open-with: e.g. open a file in an editor). */
void        nwui_spawn_arg(nwui *u, const char *cmd, const char *arg);

/* macOS-style "open": launch `path` in its associated application (a .nxe runs directly; other
 * files open in the app mapped to their extension). Associations come from
 * /disks/main/nanos/config/associations.conf (editable in Settings), with built-in defaults.
 * Reusable by any toolkit app — the NanOS equivalent of `open(1)` / LaunchServices. */
void        nwui_open_file(nwui *u, const char *path);
/* Resolve a (lowercase, no-dot) extension to an app name (config overrides built-ins). 1/0. */
int         nwui_assoc_lookup(const char *ext, char *out, int cap);

/* Ask the compositor to re-read its settings file and recompose (used by the Settings app,
 * right after it writes settings.yaml, so preference changes apply live). */
void        nwui_reload_settings(nwui *u);

/* ---- application menu (shown in the global macOS-style menu bar) ----
 * Declare top menus + items before nwui_run; the toolkit sends them to the compositor and
 * invokes the item's callback when the user picks it. The first menu's title is the app name. */
int         nwui_menu(nwui *u, const char *title);        /* add a top menu -> its index */
void        nwui_menu_item(nwui *u, int menu, const char *label, nwui_cb on_select, void *user);
void        nwui_menu_separator(nwui *u, int menu);

#ifdef __cplusplus
}
#endif

#endif /* NWUI_H */
