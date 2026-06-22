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

/* A widget callback: the firing node + the user pointer it was created with. */
typedef void (*nwui_cb)(nwui_node *self, void *user);

/* ---- lifecycle ---- */
/* Open a window and bind a toolkit to it (does the nw_connect + nw_create_window for you, so
 * the app never touches libnw directly). NULL on failure. */
nwui *nwui_open(const char *title, int w, int h);
/* Install the root of the widget tree (built with the component functions below). */
void  nwui_set_root(nwui *u, nwui_node *root);
/* Run the event loop: pump input, dispatch to widgets, repaint damaged nodes — until the
 * window is closed. */
void  nwui_run(nwui *u);

/* ---- components (allocated from the toolkit's node arena; do not free) ---- */
nwui_node *nwui_label(nwui *u, const char *text);
nwui_node *nwui_button(nwui *u, const char *text, nwui_cb on_click, void *user);
/* A w*h image (px = app-owned 0x00RRGGBB buffer) painted at its natural size. */
nwui_node *nwui_image(nwui *u, const uint32_t *px, int w, int h);
/* An editable field over an APP-OWNED buffer (the app reads the typed value straight from it). */
nwui_node *nwui_textfield(nwui *u, char *buf, int cap, nwui_cb on_change, void *user);

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

/* A scrollable list of rows. Items are an APP-OWNED array of strings (set with nwui_list_set).
 * A single click selects a row; a DOUBLE-click (or Enter) fires on_activate ("open"); arrows
 * move the selection and a scrollbar appears when rows overflow. The app reads which row fired
 * with nwui_list_selected. */
nwui_node *nwui_list(nwui *u, nwui_cb on_activate, void *user);
void       nwui_list_set(nwui_node *list, const char *const *items, int count);
int        nwui_list_selected(nwui_node *list);

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
