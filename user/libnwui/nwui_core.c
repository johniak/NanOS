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

nwui_node *nwui_textarea(nwui *u, char *buf, int cap, nwui_cb on_change, void *user)
{
	nwui_node *n = nwui_alloc(u, NWUI_TEXTAREA);
	n->tbuf = buf; n->tcap = cap;
	n->tlen = buf ? (int) strlen(buf) : 0;
	n->caret = 0; n->anchor = 0; n->scroll = 0;
	n->on_change = on_change; n->user = user; n->focusable = 1;
	return n;
}

nwui_node *nwui_checkbox(nwui *u, const char *label, int *value, nwui_cb on_change, void *user)
{
	nwui_node *n = nwui_alloc(u, NWUI_CHECKBOX);
	set_caption(n, label); n->vbool = value;
	n->on_click = on_change; n->user = user; n->focusable = 1;
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

nwui_node *nwui_image(nwui *u, const uint32_t *px, int w, int h)
{
	nwui_node *n = nwui_alloc(u, NWUI_IMAGE);
	n->img = px; n->pref_w = w; n->pref_h = h;   /* fixed natural size (the pref clamp in measure) */
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
	if (n->kind == NWUI_TEXTFIELD || n->kind == NWUI_TEXTAREA) {   /* set value + reset caret/scroll */
		int i = 0;
		if (text && n->tbuf)
			for (; text[i] && i < n->tcap - 1; i++)
				n->tbuf[i] = text[i];
		if (n->tbuf) n->tbuf[i] = 0;
		n->tlen = i;
		n->caret = i;
		n->anchor = i;
		n->scroll = 0;
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
	case NWUI_TEXTAREA:
		n->mw = 240; n->mh = 6 * NW_FONT_H;
		break;
	case NWUI_CHECKBOX:
		n->mw = 16 + 6 + (int) strlen(n->text) * NW_FONT_W;
		n->mh = NW_FONT_H + 4;
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
	if (u->modal) {                              /* lay the modal out centered (upper third) */
		nwui_measure(u->modal);
		int mw = u->modal->mw, mh = u->modal->mh;
		int mx = (u->win_w - mw) / 2, my = (u->win_h - mh) / 3;
		if (mx < 0) mx = 0;
		if (my < 0) my = 0;
		nwui_arrange(u->modal, mx, my, mw, mh);
		mark_all_dirty(u->modal);
	}
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

static nwui_node *first_focusable(nwui_node *n)
{
	if (!n) return 0;
	if (n->focusable) return n;
	for (int i = 0; i < n->nchild; i++) {
		nwui_node *r = first_focusable(n->child[i]);
		if (r) return r;
	}
	return 0;
}
void nwui_open_modal(nwui *u, nwui_node *subtree, nwui_cb on_close, void *user)
{
	u->modal = subtree;
	u->saved_focus = u->focus;
	if (u->focus) u->focus->focused = 0;
	u->modal_close_cb = on_close;
	u->modal_close_user = user;
	u->focus = first_focusable(subtree);
	if (u->focus) u->focus->focused = 1;
	u->layout_dirty = 1;
}
void nwui_close_modal(nwui *u)
{
	nwui_cb cb = u->modal_close_cb;
	void *usr = u->modal_close_user;
	if (u->focus) u->focus->focused = 0;
	u->modal = 0;
	u->focus = u->saved_focus;
	u->saved_focus = 0;
	if (u->focus) u->focus->focused = 1;
	u->modal_close_cb = 0;
	u->layout_dirty = 1;
	if (cb) cb(0, usr);
}
int nwui_modal_open(const nwui *u) { return u->modal != 0; }

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

/* ---- textarea (multiline) editing: shares tbuf/tlen/caret/anchor + sel_* helpers ---- */
static void ta_del_range(nwui_node *n, int lo, int hi)
{
	if (lo < 0) lo = 0;
	if (hi > n->tlen) hi = n->tlen;
	if (lo >= hi) return;
	int k = hi - lo;
	for (int i = lo; i + k <= n->tlen; i++) n->tbuf[i] = n->tbuf[i + k];
	n->tlen -= k; n->tbuf[n->tlen] = 0; n->caret = lo; n->anchor = lo;
}
static int ta_insert(nwui_node *n, char ch)
{
	if (ch != '\n' && ch != '\t' && (unsigned char) ch < 32) return 0;
	if (has_sel(n)) ta_del_range(n, sel_lo(n), sel_hi(n));
	if (n->tlen >= n->tcap - 1) return 0;
	for (int i = n->tlen; i > n->caret; i--) n->tbuf[i] = n->tbuf[i - 1];
	n->tbuf[n->caret] = ch; n->caret++; n->tlen++; n->anchor = n->caret;
	n->tbuf[n->tlen] = 0; return 1;
}
static void ta_copy(nwui *u, nwui_node *n)       /* selection (or whole buffer) -> clipboard */
{
	int lo = has_sel(n) ? sel_lo(n) : 0;
	int hi = has_sel(n) ? sel_hi(n) : n->tlen;
	int k = hi - lo;
	if (k > (int) sizeof u->clip_buf) k = (int) sizeof u->clip_buf;
	for (int i = 0; i < k; i++) u->clip_buf[i] = n->tbuf[lo + i];
	u->clip_len = k; u->clip_set = 1;
}
/* offset of the start of the logical line containing byte position p */
static int ta_line_start(const nwui_node *n, int p)
{
	while (p > 0 && n->tbuf[p - 1] != '\n') p--;
	return p;
}
/* offset of the end (the '\n' or tlen) of the line containing p */
static int ta_line_end(const nwui_node *n, int p)
{
	while (p < n->tlen && n->tbuf[p] != '\n') p++;
	return p;
}
static void ta_move(nwui_node *n, int pos, int extend)
{
	if (pos < 0) pos = 0;
	if (pos > n->tlen) pos = n->tlen;
	n->caret = pos;
	if (!extend) n->anchor = pos;
	n->dirty = 1;
}
/* move up/down one logical line, keeping the column (clamped to the target line) */
static void ta_move_vert(nwui_node *n, int dir, int extend)
{
	int ls = ta_line_start(n, n->caret);
	int col = n->caret - ls;
	int target;
	if (dir < 0) { if (ls == 0) return; target = ta_line_start(n, ls - 1); }
	else { int le = ta_line_end(n, n->caret); if (le >= n->tlen) return; target = le + 1; }
	int te = ta_line_end(n, target);
	int p = target + col; if (p > te) p = te;
	ta_move(n, p, extend);
}

void nwui_textarea_caret(nwui_node *n, int *line, int *col)
{
	int ln = 1;
	for (int i = 0; i < n->caret; i++) if (n->tbuf[i] == '\n') ln++;
	int ls = ta_line_start(n, n->caret);
	if (line) *line = ln;
	if (col) *col = n->caret - ls + 1;
}

enum { NWUI_TA_PAD = 4 };
static int ta_cols(const nwui_node *n)
{
	int c = (n->w - 2 * NWUI_TA_PAD) / NW_FONT_W;
	return c < 1 ? 1 : c;
}
/* visual rows a logical line [ls,le) occupies under the current wrap setting */
static int ta_line_rows(const nwui_node *n, int ls, int le)
{
	if (!n->wrap) return 1;
	int len = le - ls, cols = ta_cols(n);
	return len <= 0 ? 1 : (len + cols - 1) / cols;
}
static int ta_visible_rows(const nwui_node *n) { int v = n->h / NW_FONT_H; return v < 1 ? 1 : v; }
static int ta_total_rows(const nwui_node *n)
{
	int rows = 0, ls = 0;
	for (;;) {
		int le = ta_line_end(n, ls);
		rows += ta_line_rows(n, ls, le);
		if (le >= n->tlen) break;
		ls = le + 1;
	}
	return rows;
}
static int ta_caret_row(const nwui_node *n)     /* visual row index of the caret */
{
	int rows = 0, ls = 0;
	for (;;) {
		int le = ta_line_end(n, ls);
		if (n->caret <= le) {
			rows += n->wrap ? (n->caret - ls) / ta_cols(n) : 0;
			break;
		}
		rows += ta_line_rows(n, ls, le);
		if (le >= n->tlen) break;
		ls = le + 1;
	}
	return rows;
}
static void ta_scroll_to_caret(nwui_node *n)
{
	int row = ta_caret_row(n), vis = ta_visible_rows(n);
	if (row < n->scroll) n->scroll = row;
	else if (row >= n->scroll + vis) n->scroll = row - vis + 1;
	int maxs = ta_total_rows(n) - vis;
	if (maxs < 0) maxs = 0;
	if (n->scroll > maxs) n->scroll = maxs;
	if (n->scroll < 0) n->scroll = 0;
}

void nwui_textarea_set_wrap(nwui_node *n, int on)
{
	n->wrap = on ? 1 : 0;
	n->dirty = 1;
	if (n->owner) n->owner->layout_dirty = 1;
}
int nwui_textarea_total_rows(nwui_node *n) { return ta_total_rows(n); }

/* byte offset at a pixel point inside the textarea (wrap-aware, uses scroll) */
static int ta_pos_at(const nwui_node *n, int px, int py)
{
	int row = n->scroll + (py - (n->y + NWUI_TA_PAD)) / NW_FONT_H;
	if (row < 0) row = 0;
	int col = (px - (n->x + NWUI_TA_PAD) + NW_FONT_W / 2) / NW_FONT_W;
	if (col < 0) col = 0;
	int ls = 0, rr = 0;                       /* walk visual rows to the target row */
	for (;;) {
		int le = ta_line_end(n, ls), lr = ta_line_rows(n, ls, le);
		if (rr + lr > row) {                  /* target is within this logical line */
			int within = row - rr;
			int start = ls + (n->wrap ? within * ta_cols(n) : 0);
			int rowlen = n->wrap ? ta_cols(n) : (le - ls);
			int p = start + col;
			if (p > start + rowlen) p = start + rowlen;
			if (p > le) p = le;
			return p;
		}
		rr += lr;
		if (le >= n->tlen) return n->tlen;
		ls = le + 1;
	}
}

static int ci_eq(char a, char b, int mc)
{
	if (mc) return a == b;
	if (a >= 'A' && a <= 'Z') a += 32;
	if (b >= 'A' && b <= 'Z') b += 32;
	return a == b;
}
int nwui_textarea_find(nwui_node *n, const char *needle, int matchcase, int wrap_around)
{
	int m = (int) strlen(needle);
	if (m == 0) return 0;
	for (int pass = 0; pass < (wrap_around ? 2 : 1); pass++) {
		int from = pass == 0 ? n->caret : 0;
		int to   = pass == 0 ? n->tlen  : n->caret;
		for (int i = from; i + m <= to; i++) {
			int k = 0;
			while (k < m && ci_eq(n->tbuf[i + k], needle[k], matchcase)) k++;
			if (k == m) {
				n->anchor = i; n->caret = i + m;
				ta_scroll_to_caret(n); n->dirty = 1;
				return 1;
			}
		}
	}
	return 0;
}
void nwui_textarea_goto_line(nwui_node *n, int line1)
{
	int p = 0, ln = 1;
	while (p < n->tlen && ln < line1) { if (n->tbuf[p] == '\n') ln++; p++; }
	n->caret = p; n->anchor = p;
	ta_scroll_to_caret(n); n->dirty = 1;
}
void nwui_textarea_select_all(nwui_node *n) { n->anchor = 0; n->caret = n->tlen; n->dirty = 1; }

/* ---- path helpers (pure; for the file dialog) ---- */
void nwui_path_join(const char *dir, const char *name, char *out, int cap)
{
	int n = 0;
	for (const char *p = dir; *p && n < cap - 1; p++) out[n++] = *p;
	if (n > 0 && out[n - 1] != '/' && n < cap - 1) out[n++] = '/';
	for (const char *p = name; *p && n < cap - 1; p++) out[n++] = *p;
	out[n] = 0;
}
void nwui_path_up(char *path)
{
	int n = (int) strlen(path);
	while (n > 1 && path[n - 1] == '/') n--;          /* drop trailing slash */
	while (n > 1 && path[n - 1] != '/') n--;          /* drop last component */
	while (n > 1 && path[n - 1] == '/') n--;          /* drop the slash */
	path[n] = 0;
	if (n == 0) { path[0] = '/'; path[1] = 0; }
}
void nwui_textarea_insert_text(nwui_node *n, const char *s)
{
	int changed = 0;
	for (const char *p = s; *p; p++) changed |= ta_insert(n, *p);
	if (changed) {
		ta_scroll_to_caret(n); n->dirty = 1;
		if (n->on_change) n->on_change(n, n->user);
	}
}

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

static void cb_toggle(nwui_node *n)
{
	if (n->vbool) *n->vbool = !*n->vbool;
	n->dirty = 1;
	if (n->on_click) n->on_click(n, n->user);
}

void nwui_accel(nwui *u, int ctrl, char key, int fkey, nwui_cb cb, void *user)
{
	if (u->naccel >= 24) return;
	int i = u->naccel++;
	u->accel[i].ctrl = ctrl ? 1 : 0;
	u->accel[i].key  = (key >= 'A' && key <= 'Z') ? key + 32 : key;
	u->accel[i].fkey = fkey;
	u->accel[i].cb = cb;
	u->accel[i].user = user;
}
static int accel_fire(nwui *u, const struct nw_event *ev)
{
	char ch = ev->ch;
	if (ch >= 'A' && ch <= 'Z') ch += 32;
	for (int i = 0; i < u->naccel; i++) {
		int hit = u->accel[i].fkey
		              ? (ev->code == u->accel[i].fkey)
		              : (u->accel[i].ctrl == u->ctrl_down && u->accel[i].key && u->accel[i].key == ch);
		if (hit && u->accel[i].cb) { u->accel[i].cb(0, u->accel[i].user); return 1; }
	}
	return 0;
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

		nwui_node *over = nwui_hit(u->modal ? u->modal : u->root, ev->x, ev->y);
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
			else if (over && over->kind == NWUI_TEXTAREA) {
				set_focus(u, over);
				int p = ta_pos_at(over, ev->x, ev->y);
				over->caret = p; over->anchor = p; over->dirty = 1;   /* place caret, clear sel */
			}
			else if (over && over->kind == NWUI_CHECKBOX) {
				set_focus(u, over); over->pressed = 1; over->dirty = 1;
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
		} else if (left && pleft && u->armed && u->armed->kind == NWUI_TEXTAREA) {
			u->armed->caret = ta_pos_at(u->armed, ev->x, ev->y);     /* drag-select */
			u->armed->dirty = 1;
		} else if (left && pleft && u->armed && u->armed->kind == NWUI_LIST && u->armed->sb_drag) {
			list_sb_set_from_y(u->armed, ev->y);                     /* drag the scrollbar thumb */
			u->armed->dirty = 1;
		} else if (!left && pleft) {                  /* left release edge */
			if (u->armed && u->armed->kind == NWUI_BUTTON) {
				u->armed->pressed = 0; u->armed->dirty = 1;
				if (over == u->armed && over->on_click) over->on_click(over, over->user);
			}
			if (u->armed && u->armed->kind == NWUI_CHECKBOX) {
				u->armed->pressed = 0; u->armed->dirty = 1;
				if (over == u->armed) cb_toggle(u->armed);
			}
			if (u->armed && u->armed->kind == NWUI_LIST) u->armed->sb_drag = 0;
			u->armed = 0;
		}
		u->prev_buttons = ev->buttons;
		break;
	}

	case NW_EV_KEY: {
		if (ev->code == NWUI_SC_CTRL || ev->code == NWUI_SC_RCTRL) { u->ctrl_down = ev->down; break; }
		if (!ev->down) break;
		if (accel_fire(u, ev)) break;            /* a shortcut consumed the key */
		if (u->ctrl_down) break;                 /* suppress Ctrl+<key> from inserting/navigating */
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
		if (u->focus && u->focus->kind == NWUI_CHECKBOX) {
			if (ev->ch == ' ') cb_toggle(u->focus);
			break;
		}
		if (u->focus && u->focus->kind == NWUI_TEXTAREA) {
			nwui_node *n = u->focus;
			int shift = ev->mods & 1;
			switch (ev->code) {
			case NWUI_SC_LEFT:  ta_move(n, (shift || !has_sel(n)) ? n->caret - 1 : sel_lo(n), shift); break;
			case NWUI_SC_RIGHT: ta_move(n, (shift || !has_sel(n)) ? n->caret + 1 : sel_hi(n), shift); break;
			case NWUI_SC_HOME:  ta_move(n, ta_line_start(n, n->caret), shift); break;
			case NWUI_SC_END:   ta_move(n, ta_line_end(n, n->caret), shift); break;
			case NWUI_SC_UP:    ta_move_vert(n, -1, shift); break;
			case NWUI_SC_DOWN:  ta_move_vert(n, +1, shift); break;
			case NWUI_SC_PGUP:  for (int k = 0; k < ta_visible_rows(n); k++) ta_move_vert(n, -1, shift); break;
			case NWUI_SC_PGDN:  for (int k = 0; k < ta_visible_rows(n); k++) ta_move_vert(n, +1, shift); break;
			case NWUI_SC_DEL:
				if (has_sel(n)) ta_del_range(n, sel_lo(n), sel_hi(n));
				else if (n->caret < n->tlen) ta_del_range(n, n->caret, n->caret + 1);
				n->dirty = 1; if (n->on_change) n->on_change(n, n->user);
				break;
			default:
				if (ev->ch == 8) {                       /* backspace */
					if (has_sel(n)) ta_del_range(n, sel_lo(n), sel_hi(n));
					else if (n->caret > 0) ta_del_range(n, n->caret - 1, n->caret);
					n->dirty = 1; if (n->on_change) n->on_change(n, n->user);
				} else if (ta_insert(n, ev->ch)) {
					n->dirty = 1; if (n->on_change) n->on_change(n, n->user);
				}
				break;
			}
			ta_scroll_to_caret(n);
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
		} else if (u->focus && u->focus->kind == NWUI_TEXTAREA) {
			ta_copy(u, u->focus);
			if (ev->cut && has_sel(u->focus)) {
				ta_del_range(u->focus, sel_lo(u->focus), sel_hi(u->focus));
				u->focus->dirty = 1;
				if (u->focus->on_change) u->focus->on_change(u->focus, u->focus->user);
			}
		}
		break;

	case NW_EV_PASTE:
		if (u->focus && u->focus->kind == NWUI_TEXTFIELD && ev->text) {
			int changed = 0;
			for (int i = 0; i < ev->text_len; i++)
				changed |= tf_insert(u->focus, ev->text[i]);
			if (changed) tf_changed(u->focus);
		} else if (u->focus && u->focus->kind == NWUI_TEXTAREA && ev->text) {
			int changed = 0;
			for (int i = 0; i < ev->text_len; i++)
				changed |= ta_insert(u->focus, ev->text[i]);
			if (changed) {
				ta_scroll_to_caret(u->focus); u->focus->dirty = 1;
				if (u->focus->on_change) u->focus->on_change(u->focus, u->focus->user);
			}
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

/* ---- application menu (the global menu bar) -------------------------------------- */
static void menu_copystr(char *dst, const char *s, int cap)
{
	int i = 0; if (s) for (; s[i] && i < cap - 1; i++) dst[i] = s[i]; dst[i] = 0;
}

int nwui_menu(nwui *u, const char *title)
{
	if (u->nappmenu >= 6) return -1;
	int idx = u->nappmenu++;
	menu_copystr(u->appmenu[idx].title, title, 24);
	u->appmenu[idx].nitems = 0;
	return idx;
}
void nwui_menu_item(nwui *u, int menu, const char *label, nwui_cb cb, void *user)
{
	if (menu < 0 || menu >= u->nappmenu) return;
	struct nwui_topmenu *m = &u->appmenu[menu];
	if (m->nitems >= 12) return;
	int i = m->nitems++;
	menu_copystr(m->item[i].label, label, 24);
	m->item[i].cb = cb; m->item[i].user = user;
}
void nwui_menu_separator(nwui *u, int menu) { nwui_menu_item(u, menu, "-", 0, 0); }

int nwui_menu_encode(const nwui *u, char *out, int cap)
{
	int n = 0;
	for (int mi = 0; mi < u->nappmenu; mi++) {
		if (mi && n < cap - 1) out[n++] = 0x1e;
		const struct nwui_topmenu *m = &u->appmenu[mi];
		for (const char *t = m->title; *t && n < cap - 1;) out[n++] = *t++;
		for (int ii = 0; ii < m->nitems; ii++) {
			if (n < cap - 1) out[n++] = 0x1f;
			for (const char *l = m->item[ii].label; *l && n < cap - 1;) out[n++] = *l++;
		}
	}
	out[n < cap ? n : cap - 1] = 0;
	return n;
}
void nwui_menu_dispatch(nwui *u, int menu, int item)
{
	if (menu < 0 || menu >= u->nappmenu) return;
	struct nwui_topmenu *m = &u->appmenu[menu];
	if (item < 0 || item >= m->nitems) return;
	if (m->item[item].cb) m->item[item].cb(0, m->item[item].user);   /* self=0: a menu, no node */
}

/* ---- convenience dialogs (pure composition of widgets + the modal overlay; no I/O) ---- */
static void dlg_close(nwui_node *self, void *u) { (void) self; nwui_close_modal((nwui *) u); }

void nwui_message(nwui *u, const char *title, const char *text)
{
	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(u), 14), 10);
	nwui_add(col, nwui_colors(nwui_label(u, title), 0x172130, 0));
	nwui_add(col, nwui_label(u, text));
	nwui_add(col, nwui_button(u, "OK", dlg_close, u));
	nwui_colors(col, 0, 0x00ffffff);
	nwui_open_modal(u, col, 0, 0);
}

static struct { nwui *u; nwui_cb on_ok; void *user; } g_prompt;   /* one prompt modal at a time */
static void prompt_ok(nwui_node *self, void *unused)
{
	(void) self; (void) unused;
	nwui *u = g_prompt.u;
	nwui_cb cb = g_prompt.on_ok;
	void *usr = g_prompt.user;
	nwui_close_modal(u);
	if (cb) cb(0, usr);
}
void nwui_prompt(nwui *u, const char *title, char *buf, int cap, nwui_cb on_ok, void *user)
{
	g_prompt.u = u; g_prompt.on_ok = on_ok; g_prompt.user = user;
	nwui_node *col = nwui_gap(nwui_pad(nwui_vbox(u), 14), 10);
	nwui_add(col, nwui_colors(nwui_label(u, title), 0x172130, 0));
	nwui_add(col, nwui_textfield(u, buf, cap, 0, 0));
	nwui_node *btns = nwui_gap(nwui_hbox(u), 8);
	nwui_add(btns, nwui_button(u, "OK", prompt_ok, 0));
	nwui_add(btns, nwui_button(u, "Cancel", dlg_close, u));
	nwui_add(col, btns);
	nwui_colors(col, 0, 0x00ffffff);
	nwui_open_modal(u, col, 0, 0);
}

/* ---- programmatic clipboard (so menu items can Cut/Copy/Paste the focused field) ---- */
void nwui_post_copy(nwui *u, int cut)
{
	if (!u->focus) return;
	if (u->focus->kind == NWUI_TEXTAREA) {
		ta_copy(u, u->focus);
		if (cut && has_sel(u->focus)) {
			ta_del_range(u->focus, sel_lo(u->focus), sel_hi(u->focus));
			u->focus->dirty = 1;
			if (u->focus->on_change) u->focus->on_change(u->focus, u->focus->user);
		}
	} else if (u->focus->kind == NWUI_TEXTFIELD) {
		tf_copy(u, u->focus);
		if (cut) { tf_del_sel(u->focus); tf_changed(u->focus); }
	}
}
void nwui_post_paste(nwui *u) { u->clip_get = 1; }   /* run loop -> nw_get_clipboard -> PASTE */
