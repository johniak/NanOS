/*
 * nwui_core.c — the pure core of the toolkit (see nwui_core.h). No gfx, no I/O.
 */
#include "nwui_core.h"
#include <string.h>
#include <stdarg.h>

/* mouse buttons (NW_BTN_* in the wire protocol). */
#define MOUSE_LEFT  1
#define MOUSE_RIGHT 2

/* ---- arena + builders ------------------------------------------------------------- */
void nwui_init(nwui *u)
{
	memset(u, 0, sizeof *u);
}

nwui_node *nwui_alloc(nwui *u, int kind)
{
	int i = u->nnodes < NWUI_MAX_NODES ? u->nnodes++ : NWUI_MAX_NODES - 1;  /* clamp */
	nwui_node *n = &u->nodes[i];
	memset(n, 0, sizeof *n);
	n->kind  = kind;
	n->owner = u;
	n->fg    = 0x101014;
	return n;
}

static void set_caption(nwui_node *n, const char *text)
{
	int i = 0;
	if (text)
		for (; text[i] && i < NWUI_TEXT_CAP - 1; i++)
			n->text[i] = text[i];
	n->text[i] = 0;
}

nwui_node *nwui_label(nwui *u, const char *text)
{
	nwui_node *n = nwui_alloc(u, NWUI_LABEL);
	set_caption(n, text);
	return n;
}

nwui_node *nwui_button(nwui *u, const char *text, nwui_cb on_click, void *user)
{
	nwui_node *n = nwui_alloc(u, NWUI_BUTTON);
	set_caption(n, text);
	n->on_click = on_click;
	n->user     = user;
	n->fg = 0xffffff;
	return n;
}

nwui_node *nwui_textfield(nwui *u, char *buf, int cap, nwui_cb on_change, void *user)
{
	nwui_node *n = nwui_alloc(u, NWUI_TEXTFIELD);
	n->tbuf      = buf;
	n->tcap      = cap;
	n->tlen      = buf ? (int) strlen(buf) : 0;
	n->caret     = n->tlen;
	n->anchor    = n->tlen;
	n->on_change = on_change;
	n->user      = user;
	n->focusable = 1;
	return n;
}

nwui_node *nwui_list(nwui *u, nwui_cb on_activate, void *user)
{
	nwui_node *n = nwui_alloc(u, NWUI_LIST);
	n->on_click  = on_activate;
	n->user      = user;
	n->focusable = 1;
	n->sel       = -1;
	n->last_row  = -1;           /* no prior click -> first click is never a double */
	return n;
}

void nwui_list_set(nwui_node *list, const char *const *items, int count)
{
	if (!list || list->kind != NWUI_LIST) return;
	list->items   = items;
	list->count   = count;
	list->scroll   = 0;
	list->sel      = -1;         /* a fresh model -> no carried-over selection */
	list->sb_drag  = 0;
	list->last_row = -1;         /* and no stale double-click state */
	list->dirty    = 1;
	if (list->owner) list->owner->layout_dirty = 1;
}

int nwui_list_selected(nwui_node *list)
{
	return (list && list->kind == NWUI_LIST) ? list->sel : -1;
}

static nwui_node *collect(nwui_node *n, va_list ap)
{
	nwui_node *c;
	while ((c = va_arg(ap, nwui_node *)) != 0 && n->nchild < NWUI_MAX_CHILD)
		n->child[n->nchild++] = c;
	return n;
}

nwui_node *nwui_column(nwui *u, ...)
{
	nwui_node *n = nwui_alloc(u, NWUI_COLUMN);
	va_list ap; va_start(ap, u); collect(n, ap); va_end(ap);
	return n;
}

nwui_node *nwui_row(nwui *u, ...)
{
	nwui_node *n = nwui_alloc(u, NWUI_ROW);
	va_list ap; va_start(ap, u); collect(n, ap); va_end(ap);
	return n;
}

nwui_node *nwui_box(nwui *u, nwui_node *child)
{
	nwui_node *n = nwui_alloc(u, NWUI_BOX);
	if (child) n->child[n->nchild++] = child;
	return n;
}

nwui_node *nwui_vbox(nwui *u) { return nwui_alloc(u, NWUI_COLUMN); }
nwui_node *nwui_hbox(nwui *u) { return nwui_alloc(u, NWUI_ROW); }

nwui_node *nwui_add(nwui_node *parent, nwui_node *child)
{
	if (parent && child && parent->nchild < NWUI_MAX_CHILD)
		parent->child[parent->nchild++] = child;
	return parent;
}

/* ---- setters ---------------------------------------------------------------------- */
nwui_node *nwui_pad(nwui_node *n, int pad)        { n->pad = pad; return n; }
nwui_node *nwui_gap(nwui_node *n, int gap)        { n->gap = gap; return n; }
nwui_node *nwui_flex(nwui_node *n, int weight)    { n->flex = weight; return n; }
nwui_node *nwui_size(nwui_node *n, int w, int h)  { n->pref_w = w; n->pref_h = h; return n; }
nwui_node *nwui_colors(nwui_node *n, uint32_t fg, uint32_t bg)
{
	n->fg = fg; n->bg = bg; n->has_bg = 1; return n;
}

void nwui_set_text(nwui_node *n, const char *text)
{
	if (n->kind == NWUI_TEXTFIELD) {            /* set/replace the field's value + reset caret */
		int i = 0;
		if (text && n->tbuf)
			for (; text[i] && i < n->tcap - 1; i++)
				n->tbuf[i] = text[i];
		if (n->tbuf) n->tbuf[i] = 0;
		n->tlen = i;
		n->caret = i;
		n->anchor = i;
	} else {
		set_caption(n, text);
	}
	n->dirty = 1;
	if (n->owner) n->owner->layout_dirty = 1;   /* size may have changed */
}

const char *nwui_get_text(nwui_node *n)
{
	return n->kind == NWUI_TEXTFIELD ? (n->tbuf ? n->tbuf : "") : n->text;
}

void nwui_set_root(nwui *u, nwui_node *root) { u->root = root; u->layout_dirty = 1; }

/* ---- layout (flex-lite, measure up / arrange down) -------------------------------- */
void nwui_measure(nwui_node *n)
{
	int i;
	switch (n->kind) {
	case NWUI_LABEL:
		n->mw = (int) strlen(n->text) * NW_FONT_W;
		n->mh = NW_FONT_H;
		break;
	case NWUI_BUTTON:
		n->mw = (int) strlen(n->text) * NW_FONT_W + 2 * NWUI_BTN_PADX;
		n->mh = NW_FONT_H + 2 * NWUI_BTN_PADY;
		break;
	case NWUI_TEXTFIELD:
		n->mw = NWUI_TF_DEFW;
		n->mh = NW_FONT_H + 2 * NWUI_TF_PAD;
		break;
	case NWUI_LIST:
		n->mw = 220;
		n->mh = 4 * NWUI_ROW_H;        /* default 4 visible rows; flex stretches it */
		break;
	case NWUI_ROW: {
		int sumw = 0, maxh = 0;
		for (i = 0; i < n->nchild; i++) {
			nwui_measure(n->child[i]);
			sumw += n->child[i]->mw;
			if (n->child[i]->mh > maxh) maxh = n->child[i]->mh;
		}
		if (n->nchild > 0) sumw += n->gap * (n->nchild - 1);
		n->mw = sumw + 2 * n->pad;
		n->mh = maxh + 2 * n->pad;
		break;
	}
	case NWUI_COLUMN: {
		int maxw = 0, sumh = 0;
		for (i = 0; i < n->nchild; i++) {
			nwui_measure(n->child[i]);
			if (n->child[i]->mw > maxw) maxw = n->child[i]->mw;
			sumh += n->child[i]->mh;
		}
		if (n->nchild > 0) sumh += n->gap * (n->nchild - 1);
		n->mw = maxw + 2 * n->pad;
		n->mh = sumh + 2 * n->pad;
		break;
	}
	case NWUI_BOX:
	default:
		if (n->nchild > 0) {
			nwui_measure(n->child[0]);
			n->mw = n->child[0]->mw + 2 * n->pad;
			n->mh = n->child[0]->mh + 2 * n->pad;
		} else {
			n->mw = 2 * n->pad;
			n->mh = 2 * n->pad;
		}
		break;
	}
	if (n->pref_w > 0) n->mw = n->pref_w;
	if (n->pref_h > 0) n->mh = n->pref_h;
}

void nwui_arrange(nwui_node *n, int x, int y, int w, int h)
{
	n->x = x; n->y = y; n->w = w; n->h = h;
	int ix = x + n->pad, iy = y + n->pad;
	int iw = w - 2 * n->pad, ih = h - 2 * n->pad;
	int i;

	if (n->kind == NWUI_COLUMN) {
		int used = 0, tflex = 0;
		for (i = 0; i < n->nchild; i++) { used += n->child[i]->mh; tflex += n->child[i]->flex; }
		if (n->nchild > 0) used += n->gap * (n->nchild - 1);
		int leftover = ih - used; if (leftover < 0) leftover = 0;
		int cy = iy;
		for (i = 0; i < n->nchild; i++) {
			nwui_node *c = n->child[i];
			int chh = c->mh + (c->flex > 0 && tflex > 0 ? leftover * c->flex / tflex : 0);
			nwui_arrange(c, ix, cy, iw, chh);     /* stretch cross-axis (width) */
			cy += chh + n->gap;
		}
	} else if (n->kind == NWUI_ROW) {
		int used = 0, tflex = 0;
		for (i = 0; i < n->nchild; i++) { used += n->child[i]->mw; tflex += n->child[i]->flex; }
		if (n->nchild > 0) used += n->gap * (n->nchild - 1);
		int leftover = iw - used; if (leftover < 0) leftover = 0;
		int cx = ix;
		for (i = 0; i < n->nchild; i++) {
			nwui_node *c = n->child[i];
			int chw = c->mw + (c->flex > 0 && tflex > 0 ? leftover * c->flex / tflex : 0);
			nwui_arrange(c, cx, iy, chw, ih);     /* stretch cross-axis (height) */
			cx += chw + n->gap;
		}
	} else if (n->kind == NWUI_BOX) {
		if (n->nchild > 0)
			nwui_arrange(n->child[0], ix, iy, iw, ih);
	}
}

static void mark_all_dirty(nwui_node *n)
{
	n->dirty = 1;
	for (int i = 0; i < n->nchild; i++)
		mark_all_dirty(n->child[i]);
}

void nwui_layout(nwui *u)
{
	if (!u->root)
		return;
	nwui_measure(u->root);
	nwui_arrange(u->root, 0, 0, u->win_w, u->win_h);
	mark_all_dirty(u->root);
	u->layout_dirty = 0;
}

/* ---- hit testing + event routing -------------------------------------------------- */
nwui_node *nwui_hit(nwui_node *n, int px, int py)
{
	if (!n || px < n->x || py < n->y || px >= n->x + n->w || py >= n->y + n->h)
		return 0;
	for (int i = 0; i < n->nchild; i++) {
		nwui_node *r = nwui_hit(n->child[i], px, py);
		if (r) return r;
	}
	return n;
}

static void set_focus(nwui *u, nwui_node *n)
{
	if (u->focus == n)
		return;
	if (u->focus) { u->focus->focused = 0; u->focus->dirty = 1; }
	u->focus = n;
	if (n) { n->focused = 1; n->dirty = 1; }
}

/* ---- textfield selection + editing (caret + anchor; selection = [lo,hi)) ---- */
static int sel_lo(const nwui_node *tf) { return tf->anchor < tf->caret ? tf->anchor : tf->caret; }
static int sel_hi(const nwui_node *tf) { return tf->anchor > tf->caret ? tf->anchor : tf->caret; }
static int has_sel(const nwui_node *tf) { return tf->anchor != tf->caret; }

static void tf_move(nwui_node *tf, int pos, int extend)
{
	if (pos < 0) pos = 0;
	if (pos > tf->tlen) pos = tf->tlen;
	tf->caret = pos;
	if (!extend) tf->anchor = pos;
	tf->dirty = 1;
}
static void tf_del_range(nwui_node *tf, int lo, int hi)
{
	if (lo < 0) lo = 0;
	if (hi > tf->tlen) hi = tf->tlen;
	if (lo >= hi) return;
	int n = hi - lo;
	for (int i = lo; i + n <= tf->tlen; i++) tf->tbuf[i] = tf->tbuf[i + n];
	tf->tlen -= n;
	tf->tbuf[tf->tlen] = 0;
	tf->caret = lo; tf->anchor = lo;
}
static void tf_del_sel(nwui_node *tf) { if (has_sel(tf)) tf_del_range(tf, sel_lo(tf), sel_hi(tf)); }

static int tf_insert(nwui_node *tf, char ch)
{
	if (ch < 32 || ch >= 127) return 0;
	if (has_sel(tf)) tf_del_sel(tf);
	if (tf->tlen >= tf->tcap - 1) return 0;
	for (int i = tf->tlen; i > tf->caret; i--) tf->tbuf[i] = tf->tbuf[i - 1];
	tf->tbuf[tf->caret] = ch;
	tf->caret++; tf->tlen++; tf->anchor = tf->caret;
	tf->tbuf[tf->tlen] = 0;
	return 1;
}
static int char_at_x(const nwui_node *tf, int px)
{
	int rel = px - (tf->x + NWUI_TF_PAD);
	int i = (rel + NW_FONT_W / 2) / NW_FONT_W;   /* nearest gap */
	if (i < 0) i = 0;
	if (i > tf->tlen) i = tf->tlen;
	return i;
}
static void tf_copy(nwui *u, nwui_node *tf)       /* selection (or all) -> the clipboard buffer */
{
	int lo = has_sel(tf) ? sel_lo(tf) : 0;
	int hi = has_sel(tf) ? sel_hi(tf) : tf->tlen;
	int n = hi - lo;
	if (n > (int) sizeof u->clip_buf) n = (int) sizeof u->clip_buf;
	for (int i = 0; i < n; i++) u->clip_buf[i] = tf->tbuf[lo + i];
	u->clip_len = n;
	u->clip_set = 1;
}
static void tf_changed(nwui_node *tf) { tf->dirty = 1; if (tf->on_change) tf->on_change(tf, tf->user); }

/* ---- list ---- */
static int list_visible(const nwui_node *L) { int v = L->h / NWUI_ROW_H; return v < 1 ? 1 : v; }
static int list_max_scroll(const nwui_node *L)   /* largest valid scroll (0 if everything fits) */
{
	int m = L->count - list_visible(L);
	return m > 0 ? m : 0;
}
static int list_has_sb(const nwui_node *L) { return list_max_scroll(L) > 0; }  /* overflow -> bar */

static void list_clamp_scroll(nwui_node *L)
{
	int m = list_max_scroll(L);
	if (L->scroll > m) L->scroll = m;
	if (L->scroll < 0) L->scroll = 0;
}

/* Scrollbar thumb geometry within the list rect: top y and height, in screen pixels. */
static void list_thumb(const nwui_node *L, int *ty, int *th)
{
	int track = L->h - 2;                         /* inside the 1px border */
	int t = L->count > 0 ? track * list_visible(L) / L->count : track;
	if (t < NWUI_SB_MIN) t = NWUI_SB_MIN;
	if (t > track)       t = track;
	int maxs = list_max_scroll(L);
	int span = track - t;
	int pos  = maxs > 0 ? span * L->scroll / maxs : 0;
	*ty = L->y + 1 + pos;
	*th = t;
}

static void list_sb_set_from_y(nwui_node *L, int mouse_y)   /* drag: map thumb-top to scroll */
{
	int ty, th; list_thumb(L, &ty, &th);
	int track = L->h - 2;
	int span  = track - th;
	int maxs  = list_max_scroll(L);
	int top   = mouse_y - L->sb_grab - (L->y + 1);
	L->scroll = span > 0 ? top * maxs / span : 0;
	list_clamp_scroll(L);
}

static void list_scroll_to(nwui_node *L)     /* keep the selection within the visible window */
{
	if (L->sel < 0) return;
	int vis = list_visible(L);
	if (L->sel < L->scroll)            L->scroll = L->sel;
	else if (L->sel >= L->scroll + vis) L->scroll = L->sel - vis + 1;
	if (L->scroll < 0) L->scroll = 0;
}

static void list_activate(nwui_node *L)      /* fire on_click for the current selection */
{
	if (L->sel >= 0 && L->on_click) L->on_click(L, L->user);
}

/* ---- context menu ---- */
static const char *const MENU_LABELS[NWUI_MI_COUNT] = { "Cut", "Copy", "Paste", "Select All" };

static void menu_open(nwui *u, nwui_node *tf, int x, int y)
{
	u->menu_open = 1; u->menu_target = tf; u->menu_hover = -1;
	int mh = NWUI_MI_COUNT * NWUI_MENU_ITEM_H;
	if (x + NWUI_MENU_W > u->win_w) x = u->win_w - NWUI_MENU_W;
	if (y + mh > u->win_h) y = u->win_h - mh;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	u->menu_x = x; u->menu_y = y;
	u->layout_dirty = 1;          /* simplest: repaint the frame so the popup shows/clears */
}
static void menu_close(nwui *u) { u->menu_open = 0; u->menu_target = 0; u->layout_dirty = 1; }
static int menu_item_at(const nwui *u, int x, int y)
{
	if (x < u->menu_x || x >= u->menu_x + NWUI_MENU_W) return -1;
	int rel = y - u->menu_y;
	if (rel < 0) return -1;
	int i = rel / NWUI_MENU_ITEM_H;
	return i < NWUI_MI_COUNT ? i : -1;
}
static void menu_action(nwui *u, int item)
{
	nwui_node *tf = u->menu_target;
	if (!tf) return;
	switch (item) {
	case NWUI_MI_CUT:    tf_copy(u, tf); tf_del_sel(tf); tf_changed(tf); break;
	case NWUI_MI_COPY:   tf_copy(u, tf); break;
	case NWUI_MI_PASTE:  u->clip_get = 1; break;   /* nwui.c -> nw_get_clipboard -> NW_EV_PASTE */
	case NWUI_MI_SELALL: tf->anchor = 0; tf->caret = tf->tlen; tf->dirty = 1; break;
	default: break;
	}
}

int nwui_dispatch(nwui *u, const struct nw_event *ev)
{
	switch (ev->type) {
	case NW_EV_CONFIGURE:
		u->win_w = ev->x; u->win_h = ev->y;
		u->layout_dirty = 1;
		break;

	case NW_EV_POINTER: {
		int left  = ev->buttons & MOUSE_LEFT;
		int right = ev->buttons & MOUSE_RIGHT;
		int pleft = u->prev_buttons & MOUSE_LEFT;
		int pright = u->prev_buttons & MOUSE_RIGHT;

		if (u->menu_open) {                          /* the popup eats input while open */
			int oldh = u->menu_hover;
			u->menu_hover = menu_item_at(u, ev->x, ev->y);
			if (oldh != u->menu_hover) u->layout_dirty = 1;   /* repaint hover */
			if (left && !pleft) {
				int it = menu_item_at(u, ev->x, ev->y);
				if (it >= 0) menu_action(u, it);    /* act BEFORE close (it clears the target) */
				menu_close(u);
			} else if (right && !pright) {
				menu_close(u);
			}
			u->prev_buttons = ev->buttons;
			break;
		}

		nwui_node *over = nwui_hit(u->root, ev->x, ev->y);
		if (right && !pright && over && over->kind == NWUI_TEXTFIELD) {
			set_focus(u, over);
			menu_open(u, over, ev->x, ev->y);        /* right-click -> context menu */
		} else if (left && !pleft) {                 /* left press edge */
			u->armed = over;
			if (over && over->kind == NWUI_BUTTON) { over->pressed = 1; over->dirty = 1; }
			else if (over && over->kind == NWUI_TEXTFIELD) {
				set_focus(u, over);
				int c = char_at_x(over, ev->x);
				over->caret = c; over->anchor = c; over->dirty = 1;   /* place caret, clear sel */
			}
			else if (over && over->kind == NWUI_LIST) {
				set_focus(u, over);
				int sb_x = over->x + over->w - NWUI_SB_W;
				if (list_has_sb(over) && ev->x >= sb_x) {       /* hit the scrollbar */
					int ty, th; list_thumb(over, &ty, &th);
					if (ev->y >= ty && ev->y < ty + th) {
						over->sb_grab = ev->y - ty;              /* grabbed the thumb itself */
					} else {                                     /* clicked the track: jump here */
						over->sb_grab = th / 2;                  /* center the thumb on the cursor */
						list_sb_set_from_y(over, ev->y);
					}
					over->sb_drag = 1;                           /* either way: drag to fine-tune */
					over->dirty = 1;
				} else {                                        /* hit a row in the content area */
					int row = over->scroll + (ev->y - over->y) / NWUI_ROW_H;
					if (row >= 0 && row < over->count) {
						over->sel = row; over->dirty = 1;
						int dbl = (row == over->last_row &&
						           u->now_ms - over->last_ms <= NWUI_DBL_MS);
						over->last_row = row;
						over->last_ms  = u->now_ms;
						if (dbl) {                       /* double-click -> open/run */
							over->last_row = -1;     /* reset so a 3rd click isn't a 2nd dbl */
							list_activate(over);
						}
					}
				}
			}
		} else if (left && pleft && u->armed && u->armed->kind == NWUI_TEXTFIELD) {
			u->armed->caret = char_at_x(u->armed, ev->x);            /* drag-select */
			u->armed->dirty = 1;
		} else if (left && pleft && u->armed && u->armed->kind == NWUI_LIST && u->armed->sb_drag) {
			list_sb_set_from_y(u->armed, ev->y);                     /* drag the scrollbar thumb */
			u->armed->dirty = 1;
		} else if (!left && pleft) {                  /* left release edge */
			if (u->armed && u->armed->kind == NWUI_BUTTON) {
				u->armed->pressed = 0; u->armed->dirty = 1;
				if (over == u->armed && over->on_click) over->on_click(over, over->user);
			}
			if (u->armed && u->armed->kind == NWUI_LIST) u->armed->sb_drag = 0;
			u->armed = 0;
		}
		u->prev_buttons = ev->buttons;
		break;
	}

	case NW_EV_KEY: {
		if (!ev->down) break;
		if (u->menu_open) { if (ev->code == NWUI_SC_ESC) menu_close(u); break; }
		if (u->focus && u->focus->kind == NWUI_LIST) {
			nwui_node *L = u->focus;
			if (ev->code == NWUI_SC_UP && L->sel > 0) {
				L->sel--; list_scroll_to(L); L->dirty = 1;
			} else if (ev->code == NWUI_SC_DOWN && L->sel < L->count - 1) {
				L->sel++; list_scroll_to(L); L->dirty = 1;
			} else if (ev->ch == '\n' || ev->ch == '\r') {
				list_activate(L);
			}
			break;
		}
		if (!u->focus || u->focus->kind != NWUI_TEXTFIELD) break;
		nwui_node *tf = u->focus;
		int shift = ev->mods & 1;
		switch (ev->code) {
		case NWUI_SC_LEFT:  tf_move(tf, (shift || !has_sel(tf)) ? tf->caret - 1 : sel_lo(tf), shift); break;
		case NWUI_SC_RIGHT: tf_move(tf, (shift || !has_sel(tf)) ? tf->caret + 1 : sel_hi(tf), shift); break;
		case NWUI_SC_HOME:  tf_move(tf, 0, shift); break;
		case NWUI_SC_END:   tf_move(tf, tf->tlen, shift); break;
		default:
			if (ev->ch == '\n' || ev->ch == '\r') {
				if (tf->on_change) tf->on_change(tf, tf->user);
			} else if (ev->ch == 8) {                /* backspace */
				if (has_sel(tf)) tf_del_sel(tf);
				else if (tf->caret > 0) tf_del_range(tf, tf->caret - 1, tf->caret);
				tf_changed(tf);
			} else if (tf_insert(tf, ev->ch)) {
				tf_changed(tf);
			}
			break;
		}
		break;
	}

	case NW_EV_COPY:
		if (u->focus && u->focus->kind == NWUI_TEXTFIELD) {
			tf_copy(u, u->focus);                    /* nwui.c forwards clip_buf to the server */
			if (ev->cut) { tf_del_sel(u->focus); tf_changed(u->focus); }
		}
		break;

	case NW_EV_PASTE:
		if (u->focus && u->focus->kind == NWUI_TEXTFIELD && ev->text) {
			int changed = 0;
			for (int i = 0; i < ev->text_len; i++)
				changed |= tf_insert(u->focus, ev->text[i]);
			if (changed) tf_changed(u->focus);
		}
		break;

	case NW_EV_CLOSE:
		u->closed = 1;
		return 0;
	default:
		break;
	}
	return 1;
}
