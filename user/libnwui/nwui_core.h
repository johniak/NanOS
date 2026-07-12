/*
 * nwui_core.h — the internal model of the toolkit (full structs). The pure core: node tree,
 * arena, flex-lite layout (measure/arrange), hit-testing, event routing, textfield editing,
 * dirty tracking. NO gfx and NO I/O — host-tested. nwui_paint.c paints; nwui.c does I/O.
 */
#ifndef NWUI_CORE_H
#define NWUI_CORE_H

#include <stdint.h>
#include "nwui.h"
#include "libnw.h"     /* struct nw_event + NW_EV_* / NW_BTN_* — the core only READS events */

enum { NWUI_BOX, NWUI_ROW, NWUI_COLUMN, NWUI_LABEL, NWUI_BUTTON, NWUI_TEXTFIELD, NWUI_LIST, NWUI_IMAGE,
       NWUI_TEXTAREA, NWUI_CHECKBOX, NWUI_ICONVIEW, NWUI_PANEL };

enum { NWUI_ROW_H = 18 };   /* list item row height */
enum { NWUI_SB_W = 12, NWUI_SB_MIN = 16 };   /* list scrollbar: width, min thumb height */
enum { NWUI_DBL_MS = 400 };   /* two clicks on the same row within this -> a double-click */

enum { NWUI_ICON_CELL_W = 120, NWUI_ICON_CELL_H = 108 };  /* icon-grid cell box (modern, roomy) */
enum { NWUI_ICON_PX = 64 };                               /* nominal icon size (authored 64x64) */
enum { NWUI_PANEL_TITLE_H = 22 };                         /* titled-panel header band height */
enum { NWUI_LINK_H = 30 };                                /* sidebar link/nav-row height */

enum {
	NWUI_MAX_NODES = 128,
	NWUI_MAX_CHILD = 16,
	NWUI_TEXT_CAP  = 64,
	NWUI_BTN_PADX  = 10,   /* button text inset */
	NWUI_BTN_PADY  = 4,
	NWUI_TF_PAD    = 4,    /* textfield inset */
	NWUI_TF_DEFW   = 140   /* default textfield width */
};

struct nwui_node {
	int        kind;
	nwui      *owner;
	int        x, y, w, h;        /* final rect (arrange) */
	int        mw, mh;            /* measured size (measure) */
	int        pref_w, pref_h;    /* explicit fixed size (0 = auto) */
	int        flex;             /* main-axis grow weight in parent */
	int        pad, gap;
	uint32_t   fg, bg;
	int        has_bg;

	nwui_node *child[NWUI_MAX_CHILD];
	int        nchild;

	char       text[NWUI_TEXT_CAP];   /* label / button caption */
	char      *tbuf;                  /* textfield: app-owned buffer */
	int        tcap, tlen, caret;
	int        anchor;               /* selection anchor; selection = [min,max) when != caret */

	int        wrap;                 /* textarea: word-wrap on/off */
	int       *vbool;                /* checkbox: app-owned bound flag */

	const uint32_t *img;             /* image: app-owned w*h pixel buffer (0x00RRGGBB) */
	const char *const *items;        /* list: app-owned array of item strings */
	int        count, sel, scroll;   /* list/iconview: item count, selected, visible-from index */
	int        sb_drag, sb_grab;     /* list: scrollbar thumb being dragged + grab offset (px) */
	int        last_row, last_ms;    /* list/iconview: last clicked cell + time, for double-click */

	const nwui_icon_item *icons;     /* iconview: app-owned array of cells (reuses count/sel/scroll) */
	int        cols;                 /* iconview: column count computed at arrange time */
	unsigned char *selmask;          /* iconview: 1 byte/cell, 1 = in the multi-selection (NULL = none) */
	int        selcap;               /* iconview: allocated capacity of selmask, in cells */
	int        sel_anchor;           /* iconview: Shift-range anchor cell (the last plain/Cmd click) */
	int        flat;                 /* button: render as a flat sidebar link (no gradient fill) */
	int        active;               /* flat link: render as a filled accent pill (current location) */

	nwui_cb    on_click, on_change;
	void      *user;

	/* drag-and-drop (iconview): gesture-started-a-drag / a-drop-landed callbacks, the cell
	 * currently highlighted as a hovering drop target, and the landed drop's cell + payload. */
	nwui_cb    on_drag, on_drop;
	nwui_cb    on_submit;            /* textfield: fired on Enter (distinct from on_change)  */
	nwui_cb    on_copy, on_paste;    /* iconview: Cmd+C/X (copy/cut) and Cmd+V (paste) events */
	int        copy_cut;             /* iconview: 1 if the last on_copy was a cut (Cmd+X)    */
	int        drop_hover;           /* cell under a hovering drag, or -1                  */
	int        drop_cell;            /* cell a drop landed on, or -1 (= the empty area)     */
	int        drop_mods;            /* modifier bits at the drop (bit0 shift, bit1 ctrl)   */
	const char *drop_text;           /* dropped payload (valid only during on_drop)         */

	int        focusable, focused, hover, pressed, dirty;
	int        hidden;               /* nwui_set_visible(n,0): skipped in layout/paint/hit (tabs) */
	int        secret;               /* textfield: render the value as dots (password entry) */
};

/* A top menu in the application menu bar (file-scope so C++ host tests see the tag). */
struct nwui_topmenu {
	char title[24];
	struct { char label[24]; nwui_cb cb; void *user; } item[12];
	int  nitems;
};

struct nwui {
	struct nwui_node nodes[NWUI_MAX_NODES];
	int        nnodes;
	nwui_node *root;
	nwui_node *focus;             /* focused textfield, or NULL */
	nwui_node *armed;             /* node a left-press landed on (for click-on-release) */
	int        win_w, win_h;
	int        glass;             /* light-glass interior mode (NW_STYLE_GLASS_CLIENT at open) */
	int        dark;              /* dark-slab window (NW_STYLE_DARK): glass paints light ink  */
	int        now_ms;            /* current time (ms) injected by the I/O shell before dispatch */
	int        prev_buttons;
	int        layout_dirty;      /* tree/sizes changed -> full relayout + repaint */
	void      *io;                /* nwui.c stashes its nw_display + nw_win here; core ignores it */
	int        closed;

	/* drag-and-drop: the core flags a pending nw_drag_begin (I/O shell sends it), tracks the
	 * iconview cell pressed (a candidate that becomes a drag once the cursor moves past a
	 * threshold), and the iconview currently showing a drop-target highlight. */
	int        drag_req;          /* set -> nwui.c does nw_drag_begin(drag_buf, drag_len)      */
	char       drag_buf[256];
	int        drag_len;
	nwui_node *drag_cand;         /* iconview cell under a left-press (potential drag source)  */
	int        drag_x0, drag_y0;  /* press point, for the move threshold                      */
	nwui_node *drop_node;         /* iconview currently highlighted as a drop target          */

	/* clipboard hand-off to the I/O shell (nwui.c performs the actual nw_* call) */
	int        clip_set;          /* set -> nwui.c does nw_set_clipboard(clip_buf, clip_len)  */
	int        clip_get;          /* set -> nwui.c does nw_get_clipboard() (reply = a PASTE)   */
	char       clip_buf[256];
	int        clip_len;

	/* context-menu overlay (a popup over the window; v1: the textfield's Cut/Copy/Paste/All) */
	int        menu_open, menu_x, menu_y, menu_hover;
	nwui_node *menu_target;
	/* a custom app context menu (shown on right-click of an iconview). menu_custom selects it
	 * over the built-in textfield menu while a popup is open. */
	int         menu_custom;
	const char *cmenu_label[16];
	nwui_cb     cmenu_cb[16];
	void       *cmenu_user[16];
	int         cmenu_n;

	/* global (menu-bar) menus the app declares; sent to the compositor via nw_set_menu */
	struct nwui_topmenu appmenu[6];
	int nappmenu;

	/* keyboard accelerators (Ctrl+<letter> or a function key) + tracked Ctrl state */
	int ctrl_down;
	struct { int ctrl, fkey; char key; nwui_cb cb; void *user; } accel[24];
	int naccel;

	/* modal overlay: a centered sub-tree that captures all input until dismissed */
	nwui_node *modal;            /* modal subtree root, or NULL */
	nwui_node *saved_focus;      /* focus to restore on close */
	nwui_cb    modal_close_cb;
	void      *modal_close_user;
	/* default action fired by Enter while the modal is open (e.g. the prompt's OK / confirm's
	 * affirmative button), so dialogs are keyboard-complete. NULL = Enter does nothing. */
	nwui_cb    modal_default;
	void      *modal_default_user;
};

/* Built-in context-menu items (indices). */
enum { NWUI_MI_CUT, NWUI_MI_COPY, NWUI_MI_PASTE, NWUI_MI_SELALL, NWUI_MI_COUNT };
enum { NWUI_MENU_W = 110, NWUI_MENU_ITEM_H = 20 };

/* Normalised scancodes for caret/selection (bit7=extended on the wire). */
enum {
	NWUI_SC_LEFT  = 0xCB, NWUI_SC_RIGHT = 0xCD,
	NWUI_SC_UP    = 0xC8, NWUI_SC_DOWN  = 0xD0,
	NWUI_SC_HOME  = 0xC7, NWUI_SC_END   = 0xCF, NWUI_SC_ESC = 0x01,
	NWUI_SC_PGUP  = 0xC9, NWUI_SC_PGDN  = 0xD1, NWUI_SC_DEL = 0xD3,
	NWUI_SC_CTRL  = 0x1D, NWUI_SC_RCTRL = 0x9D, NWUI_SC_F3  = 0x3D, NWUI_SC_F5 = 0x3F
};

/* ---- core (pure) ---- */
void       nwui_init(nwui *u);                       /* zero + defaults (no I/O) */
nwui_node *nwui_alloc(nwui *u, int kind);            /* arena node, zeroed, owner set */

void       nwui_measure(nwui_node *n);               /* bottom-up sizes into mw/mh */
void       nwui_arrange(nwui_node *n, int x, int y, int w, int h);  /* top-down rects */
void       nwui_layout(nwui *u);                     /* measure(root)+arrange to window */

nwui_node *nwui_hit(nwui_node *n, int px, int py);   /* deepest node under the point, or NULL */
int        nwui_dispatch(nwui *u, const struct nw_event *ev);  /* route one event; 0 = closed */

/* ---- application menu (the global menu bar) ---- */
int        nwui_menu(nwui *u, const char *title);                       /* add top menu -> index */
void       nwui_menu_item(nwui *u, int menu, const char *label, nwui_cb cb, void *user);
/* custom right-click context menu (e.g. on an iconview): clear, then add items; it pops up
 * automatically when an iconview is right-clicked. cb(0, user) fires on the chosen item. */
void       nwui_context_clear(nwui *u);
void       nwui_context_add(nwui *u, const char *label, nwui_cb cb, void *user);
void       nwui_menu_separator(nwui *u, int menu);
int        nwui_menu_encode(const nwui *u, char *out, int cap);         /* -> wire spec; len */
void       nwui_menu_dispatch(nwui *u, int menu, int item);             /* invoke the callback */

/* ---- paint (nwui_paint.c, uses nw_gfx) ---- */
/* Render the tree into surface `s`: full repaint on layout change, else only dirty nodes.
 * Writes the damaged rect to *x,*y,*w,*h and returns 1 if anything was painted, else 0. */
int nwui_render(nwui *u, const struct nw_surface *s, int *x, int *y, int *w, int *h);

#endif /* NWUI_CORE_H */
