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
       NWUI_TEXTAREA, NWUI_CHECKBOX };

enum { NWUI_ROW_H = 18 };   /* list item row height */
enum { NWUI_SB_W = 12, NWUI_SB_MIN = 16 };   /* list scrollbar: width, min thumb height */
enum { NWUI_DBL_MS = 400 };   /* two clicks on the same row within this -> a double-click */

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
	int        count, sel, scroll;   /* list: item count, selected/visible-from index */
	int        sb_drag, sb_grab;     /* list: scrollbar thumb being dragged + grab offset (px) */
	int        last_row, last_ms;    /* list: last clicked row + time, for double-click detect */

	nwui_cb    on_click, on_change;
	void      *user;

	int        focusable, focused, hover, pressed, dirty;
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
	int        now_ms;            /* current time (ms) injected by the I/O shell before dispatch */
	int        prev_buttons;
	int        layout_dirty;      /* tree/sizes changed -> full relayout + repaint */
	void      *io;                /* nwui.c stashes its nw_display + nw_win here; core ignores it */
	int        closed;

	/* clipboard hand-off to the I/O shell (nwui.c performs the actual nw_* call) */
	int        clip_set;          /* set -> nwui.c does nw_set_clipboard(clip_buf, clip_len)  */
	int        clip_get;          /* set -> nwui.c does nw_get_clipboard() (reply = a PASTE)   */
	char       clip_buf[256];
	int        clip_len;

	/* context-menu overlay (a popup over the window; v1: the textfield's Cut/Copy/Paste/All) */
	int        menu_open, menu_x, menu_y, menu_hover;
	nwui_node *menu_target;

	/* global (menu-bar) menus the app declares; sent to the compositor via nw_set_menu */
	struct nwui_topmenu appmenu[6];
	int nappmenu;
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
void       nwui_menu_separator(nwui *u, int menu);
int        nwui_menu_encode(const nwui *u, char *out, int cap);         /* -> wire spec; len */
void       nwui_menu_dispatch(nwui *u, int menu, int item);             /* invoke the callback */

/* ---- paint (nwui_paint.c, uses nw_gfx) ---- */
/* Render the tree into surface `s`: full repaint on layout change, else only dirty nodes.
 * Writes the damaged rect to *x,*y,*w,*h and returns 1 if anything was painted, else 0. */
int nwui_render(nwui *u, const struct nw_surface *s, int *x, int *y, int *w, int *h);

#endif /* NWUI_CORE_H */
