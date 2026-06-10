/*
 * nwui_core.c — the pure core of the toolkit (see nwui_core.h). No gfx, no I/O.
 */
#include "nwui_core.h"
#include <string.h>
#include <stdarg.h>

/* left mouse button = bit 0 (NW_BTN_LEFT in the wire protocol). */
#define MOUSE_LEFT 1

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
	n->on_change = on_change;
	n->user      = user;
	n->focusable = 1;
	return n;
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

/* Edit the focused textfield with one ASCII char (0 if none). Returns 1 if changed. */
static int tf_edit(nwui_node *tf, char ch)
{
	if (ch == 8) {                       /* backspace */
		if (tf->caret <= 0) return 0;
		for (int i = tf->caret - 1; i < tf->tlen - 1; i++) tf->tbuf[i] = tf->tbuf[i + 1];
		tf->caret--; tf->tlen--;
		tf->tbuf[tf->tlen] = 0;
		return 1;
	}
	if (ch >= 32 && ch < 127) {          /* printable insert at caret */
		if (tf->tlen >= tf->tcap - 1) return 0;
		for (int i = tf->tlen; i > tf->caret; i--) tf->tbuf[i] = tf->tbuf[i - 1];
		tf->tbuf[tf->caret] = ch;
		tf->caret++; tf->tlen++;
		tf->tbuf[tf->tlen] = 0;
		return 1;
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
		int left = ev->buttons & MOUSE_LEFT;
		int prev = u->prev_buttons & MOUSE_LEFT;
		nwui_node *over = nwui_hit(u->root, ev->x, ev->y);
		if (left && !prev) {                         /* press edge */
			u->armed = over;
			if (over && over->kind == NWUI_BUTTON) { over->pressed = 1; over->dirty = 1; }
		} else if (!left && prev) {                  /* release edge */
			if (u->armed && u->armed->kind == NWUI_BUTTON) { u->armed->pressed = 0; u->armed->dirty = 1; }
			if (u->armed && over == u->armed) {
				if (over->kind == NWUI_BUTTON) {
					if (over->on_click) over->on_click(over, over->user);
				} else if (over->kind == NWUI_TEXTFIELD) {
					set_focus(u, over);
				}
			}
			u->armed = 0;
		}
		u->prev_buttons = ev->buttons;
		break;
	}

	case NW_EV_KEY:
		if (ev->down && u->focus && u->focus->kind == NWUI_TEXTFIELD) {
			if (ev->ch == '\n' || ev->ch == '\r') {
				if (u->focus->on_change) u->focus->on_change(u->focus, u->focus->user);
			} else if (tf_edit(u->focus, ev->ch)) {
				u->focus->dirty = 1;
				if (u->focus->on_change) u->focus->on_change(u->focus, u->focus->user);
			}
		}
		break;

	case NW_EV_PASTE:
		if (u->focus && u->focus->kind == NWUI_TEXTFIELD && ev->text) {
			int changed = 0;
			for (int i = 0; i < ev->text_len; i++)
				changed |= tf_edit(u->focus, ev->text[i]);
			if (changed) {
				u->focus->dirty = 1;
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
