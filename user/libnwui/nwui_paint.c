/*
 * nwui_paint.c — render the widget tree into a window surface via nw_gfx. Full repaint on a
 * layout change, otherwise only dirty nodes (-> a small damage rect the compositor blits).
 */
#include "nwui_core.h"
#include "nw_gfx.h"
#include <string.h>

#define COL_WIN     0x00f4f4ec   /* window background (paper) */
#define COL_BTN     0x003a6ea5
#define COL_BTN_DN  0x002a4a78   /* pressed */
#define COL_TF_BG   0x00ffffff
#define COL_TF_BRD  0x00808080
#define COL_TF_FOC  0x003a6ea5
#define COL_INK     0x00101014

static void paint_self(nwui_node *n, const struct nw_surface *s)
{
	switch (n->kind) {
	case NWUI_LABEL:
		nw_draw_text(s, n->x, n->y, n->text, n->fg, COL_WIN);
		break;
	case NWUI_BUTTON: {
		uint32_t bg = n->has_bg ? n->bg : (n->pressed ? COL_BTN_DN : COL_BTN);
		nw_fill_rect(s, n->x, n->y, n->w, n->h, bg);
		int tx = n->x + (n->w - (int) strlen(n->text) * NW_FONT_W) / 2;
		int ty = n->y + (n->h - NW_FONT_H) / 2;
		nw_draw_text(s, tx, ty, n->text, n->fg, bg);
		break;
	}
	case NWUI_TEXTFIELD: {
		nw_fill_rect(s, n->x, n->y, n->w, n->h, n->focused ? COL_TF_FOC : COL_TF_BRD);
		nw_fill_rect(s, n->x + 1, n->y + 1, n->w - 2, n->h - 2, COL_TF_BG);
		int tx = n->x + NWUI_TF_PAD, ty = n->y + (n->h - NW_FONT_H) / 2;
		int lo = n->anchor < n->caret ? n->anchor : n->caret;
		int hi = n->anchor > n->caret ? n->anchor : n->caret;
		for (int i = 0; i < n->tlen; i++) {                 /* per-char so selection inverts */
			int sel = (n->anchor != n->caret && i >= lo && i < hi);
			nw_draw_char(s, tx + i * NW_FONT_W, ty, (unsigned char) n->tbuf[i],
			             sel ? 0x00ffffff : COL_INK, sel ? COL_TF_FOC : COL_TF_BG);
		}
		if (n->focused)
			nw_fill_rect(s, tx + n->caret * NW_FONT_W, ty, 1, NW_FONT_H, COL_INK);
		break;
	}
	case NWUI_LIST: {
		nw_fill_rect(s, n->x, n->y, n->w, n->h, n->focused ? COL_TF_FOC : COL_TF_BRD);
		nw_fill_rect(s, n->x + 1, n->y + 1, n->w - 2, n->h - 2, COL_TF_BG);   /* inner paper */
		int maxs = n->count - n->h / NWUI_ROW_H;
		int has_sb = maxs > 0;
		int roww = n->w - 2 - (has_sb ? NWUI_SB_W : 0);    /* rows stop before the scrollbar */
		int vis = n->h / NWUI_ROW_H;
		for (int r = 0; r < vis; r++) {
			int idx = n->scroll + r;
			if (idx >= n->count) break;
			int ry = n->y + 1 + r * NWUI_ROW_H;
			int seld = (idx == n->sel);
			uint32_t bg = seld ? COL_TF_FOC : COL_TF_BG;
			uint32_t fg = seld ? 0x00ffffff : COL_INK;
			if (seld) nw_fill_rect(s, n->x + 1, ry, roww, NWUI_ROW_H, bg);
			if (n->items && n->items[idx])
				nw_draw_text(s, n->x + NWUI_TF_PAD, ry + (NWUI_ROW_H - NW_FONT_H) / 2,
				             n->items[idx], fg, bg);
		}
		if (has_sb) {                                      /* vertical scrollbar: track + thumb */
			int sbx = n->x + n->w - NWUI_SB_W;
			nw_fill_rect(s, sbx, n->y + 1, NWUI_SB_W - 1, n->h - 2, 0x00d8d8d0);  /* track */
			int track = n->h - 2;
			int th = track * vis / n->count; if (th < NWUI_SB_MIN) th = NWUI_SB_MIN;
			if (th > track) th = track;
			int ty = n->y + 1 + (maxs > 0 ? (track - th) * n->scroll / maxs : 0);
			nw_fill_rect(s, sbx + 1, ty, NWUI_SB_W - 3, th, COL_TF_FOC);          /* thumb */
		}
		break;
	}
	default:   /* row/column/box: paint own background if set (else transparent) */
		if (n->has_bg)
			nw_fill_rect(s, n->x, n->y, n->w, n->h, n->bg);
		break;
	}
}

/* pre-order so parents paint before children (children on top) */
static void paint_all(nwui_node *n, const struct nw_surface *s)
{
	paint_self(n, s);
	for (int i = 0; i < n->nchild; i++)
		paint_all(n->child[i], s);
}
static void clear_dirty(nwui_node *n)
{
	n->dirty = 0;
	for (int i = 0; i < n->nchild; i++)
		clear_dirty(n->child[i]);
}

struct dmg { int have, x0, y0, x1, y1; };
static void repaint_dirty(nwui_node *n, const struct nw_surface *s, struct dmg *d)
{
	if (n->dirty) {
		paint_self(n, s);
		if (!d->have) { d->x0 = n->x; d->y0 = n->y; d->x1 = n->x + n->w; d->y1 = n->y + n->h; d->have = 1; }
		else {
			if (n->x < d->x0) d->x0 = n->x;
			if (n->y < d->y0) d->y0 = n->y;
			if (n->x + n->w > d->x1) d->x1 = n->x + n->w;
			if (n->y + n->h > d->y1) d->y1 = n->y + n->h;
		}
		n->dirty = 0;
	}
	for (int i = 0; i < n->nchild; i++)
		repaint_dirty(n->child[i], s, d);
}

/* the context-menu overlay, drawn last (on top of everything) */
static void draw_menu(const nwui *u, const struct nw_surface *s)
{
	if (!u->menu_open)
		return;
	static const char *const L[NWUI_MI_COUNT] = { "Cut", "Copy", "Paste", "Select All" };
	int mh = NWUI_MI_COUNT * NWUI_MENU_ITEM_H;
	nw_fill_rect(s, u->menu_x - 1, u->menu_y - 1, NWUI_MENU_W + 2, mh + 2, 0x00101418);
	for (int i = 0; i < NWUI_MI_COUNT; i++) {
		int iy = u->menu_y + i * NWUI_MENU_ITEM_H;
		uint32_t bg = (i == u->menu_hover) ? COL_TF_FOC : 0x00f4f4ec;
		uint32_t fg = (i == u->menu_hover) ? 0x00ffffff : COL_INK;
		nw_fill_rect(s, u->menu_x, iy, NWUI_MENU_W, NWUI_MENU_ITEM_H, bg);
		nw_draw_text(s, u->menu_x + 8, iy + (NWUI_MENU_ITEM_H - NW_FONT_H) / 2, L[i], fg, bg);
	}
}

int nwui_render(nwui *u, const struct nw_surface *s, int *x, int *y, int *w, int *h)
{
	if (!u->root)
		return 0;
	if (u->layout_dirty) {
		nwui_layout(u);
		nw_fill_rect(s, 0, 0, u->win_w, u->win_h, COL_WIN);
		paint_all(u->root, s);
		draw_menu(u, s);
		clear_dirty(u->root);
		*x = 0; *y = 0; *w = u->win_w; *h = u->win_h;
		return 1;
	}
	struct dmg d = { 0, 0, 0, 0, 0 };
	repaint_dirty(u->root, s, &d);
	if (!d.have)
		return 0;
	*x = d.x0; *y = d.y0; *w = d.x1 - d.x0; *h = d.y1 - d.y0;
	return 1;
}
