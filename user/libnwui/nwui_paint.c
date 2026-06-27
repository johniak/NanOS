/*
 * nwui_paint.c — render the widget tree into a window surface via nw_gfx. Full repaint on a
 * layout change, otherwise only dirty nodes (-> a small damage rect the compositor blits).
 */
#include "nwui_core.h"
#include "nw_gfx.h"
#include <string.h>

#define COL_WIN     0x00f7fafe   /* window content paper (matches the compositor material) */
#define COL_BTN_TOP 0x0039b4f7   /* button gradient (blue) */
#define COL_BTN_BOT 0x001f8fe0
#define COL_BTN_DTOP 0x002a78b0  /* pressed */
#define COL_BTN_DBOT 0x00165f95
#define COL_TF_BG   0x00ffffff
#define COL_TF_BRD  0x00cdd7e5
#define COL_TF_FOC  0x0012a8f4   /* accent: focus ring + selection */
#define COL_INK     0x001c1c1e
#define COL_MUTED   0x008a8a8e
#define COL_ACCENT      0x000a84ff   /* modern azure accent (selection, links, pill) */
#define COL_ACCENT_DEEP 0x000060df
#define COL_SEL     0x000a84ff
#define COL_SB_THUMB 0x00c2c8d2
#define COL_PANEL_BG  0x00eef4fb   /* task-pane panel body (light glass) */
#define COL_PANEL_HDR 0x000a84ff   /* task-pane panel title band (accent) */
#define COL_ICON_SHADOW 0x00102038 /* soft drop shadow under grid icons */
#define NWUI_ICON_KEY 0x00ff00ff   /* icon transparency color-key (magenta); generator must match */

static void paint_self(nwui_node *n, const struct nw_surface *s)
{
	switch (n->kind) {
	case NWUI_LABEL:
		nw_text(s, n->x, n->y, n->text, n->fg ? n->fg : COL_INK);
		break;
	case NWUI_IMAGE:
		if (n->img) {
			/* Blit at the image's NATURAL size (pref_w/pref_h), not n->w/n->h — the column/row
			 * layout may have stretched the node, and the pixel buffer is only pref_w*pref_h, so
			 * using n->w as the stride/extent would read far past it (a crash). */
			int iw = n->pref_w, ih = n->pref_h;
			struct nw_surface src;
			src.px = (uint32_t *) n->img; src.w = iw; src.h = ih; src.stride = iw;
			nw_surface_noclip(&src);
			nw_blit(s, n->x, n->y, &src, 0, 0, iw, ih);
		}
		break;
	case NWUI_BUTTON: {
		if (n->flat && n->img) {               /* toolbar icon button */
			if (n->pressed)
				nw_fill_round(s, n->x, n->y + 1, n->w, n->h - 2, 7, COL_ACCENT, 28);
			int iw = n->count, ih = n->sel;     /* native icon size (stashed by nwui_iconbtn) */
			int ix = n->x + (n->w - iw) / 2, iy = n->y + (n->h - ih) / 2;
			for (int yy = 0; yy < ih; yy++)
				for (int xx = 0; xx < iw; xx++) {
					uint32_t p = n->img[yy * iw + xx];
					int a = (int) (p >> 24);
					if (a) nw_blend_pixel(s, ix + xx, iy + yy, p & 0x00ffffff, a);
				}
			break;
		}
		if (n->flat) {                         /* sidebar link / nav-row */
			int ty = n->y + (n->h - NW_FONT_H) / 2;
			if (n->active) {                   /* current location -> filled accent pill */
				nw_fill_round(s, n->x, n->y + 2, n->w, n->h - 4, 8, COL_ACCENT, 255);
				nw_text(s, n->x + 12, ty, n->text, 0x00ffffff);
			} else {
				if (n->pressed)
					nw_fill_round(s, n->x, n->y + 2, n->w, n->h - 4, 8, COL_ACCENT, 30);
				nw_text(s, n->x + 12, ty, n->text, n->fg ? n->fg : COL_INK);
			}
			break;
		}
		int down = n->pressed;
		uint32_t base = n->has_bg ? n->bg : (down ? COL_BTN_DBOT : COL_BTN_BOT);
		nw_fill_round(s, n->x, n->y, n->w, n->h, 7, base, 255);        /* rounded solid fill */
		if (!n->has_bg)                                                /* glossy top sheen */
			nw_blend_rect(s, n->x + 3, n->y + 2, n->w - 6, (n->h - 4) / 2,
			              down ? COL_BTN_DTOP : COL_BTN_TOP, 120);
		nw_stroke_round(s, n->x, n->y, n->w, n->h, 7, 0x00ffffff, 60);
		int tx = n->x + (n->w - nw_text_w(n->text)) / 2;
		int ty = n->y + (n->h - NW_FONT_H) / 2;
		nw_text(s, tx, ty, n->text, n->fg ? n->fg : 0x00ffffff);
		break;
	}
	case NWUI_TEXTFIELD: {
		nw_fill_round(s, n->x, n->y, n->w, n->h, 6, COL_TF_BG, 255);
		nw_stroke_round(s, n->x, n->y, n->w, n->h, 6, n->focused ? COL_TF_FOC : COL_TF_BRD,
		                n->focused ? 255 : 200);
		if (n->focused)                                     /* a second ring = a soft 2px focus */
			nw_stroke_round(s, n->x + 1, n->y + 1, n->w - 2, n->h - 2, 5, COL_TF_FOC, 120);
		int tx = n->x + NWUI_TF_PAD, ty = n->y + (n->h - NW_FONT_H) / 2;
		int lo = n->anchor < n->caret ? n->anchor : n->caret;
		int hi = n->anchor > n->caret ? n->anchor : n->caret;
		for (int i = 0; i < n->tlen; i++) {
			int sel = (n->anchor != n->caret && i >= lo && i < hi);
			if (sel) nw_fill_rect(s, tx + i * NW_FONT_W, ty, NW_FONT_W, NW_FONT_H, COL_SEL);
			nw_draw_char_t(s, tx + i * NW_FONT_W, ty, (unsigned char) n->tbuf[i],
			               sel ? 0x00ffffff : COL_INK);
		}
		if (n->focused)
			nw_fill_rect(s, tx + n->caret * NW_FONT_W, ty, 2, NW_FONT_H, COL_TF_FOC);
		break;
	}
	case NWUI_TEXTAREA: {
		nw_fill_round(s, n->x, n->y, n->w, n->h, 6, COL_TF_BG, 255);
		int pad = 4;
		int cols = (n->w - 2 * pad) / NW_FONT_W; if (cols < 1) cols = 1;
		int vis  = n->h / NW_FONT_H;             if (vis  < 1) vis  = 1;
		int lo = n->anchor < n->caret ? n->anchor : n->caret;
		int hi = n->anchor > n->caret ? n->anchor : n->caret;
		int tx0 = n->x + pad, ty0 = n->y + pad;
		int caret_px = -1, caret_py = -1;
		int ls = 0, vrow = 0, drawn = 0;
		if (n->tbuf) for (;;) {
			int lend = ls;
			while (lend < n->tlen && n->tbuf[lend] != '\n') lend++;   /* logical line [ls,lend) */
			int seg = ls;
			do {                                  /* one or more visual rows per logical line */
				int segend = n->wrap ? (seg + cols < lend ? seg + cols : lend) : lend;
				if (vrow >= n->scroll && drawn < vis) {
					int yy = ty0 + drawn * NW_FONT_H;
					for (int i = seg; i < segend; i++) {
						int seld = (n->anchor != n->caret && i >= lo && i < hi);
						int xx = tx0 + (i - seg) * NW_FONT_W;
						if (seld) nw_fill_rect(s, xx, yy, NW_FONT_W, NW_FONT_H, COL_SEL);
						nw_draw_char_t(s, xx, yy, (unsigned char) n->tbuf[i],
						               seld ? 0x00ffffff : COL_INK);
					}
					if (n->focused && n->caret >= seg && n->caret <= segend) {
						caret_px = tx0 + (n->caret - seg) * NW_FONT_W;
						caret_py = yy;
					}
					drawn++;
				}
				vrow++;
				seg = segend;
			} while (n->wrap && seg < lend);
			if (lend >= n->tlen) break;
			ls = lend + 1;
		}
		if (caret_px >= 0) nw_fill_rect(s, caret_px, caret_py, 2, NW_FONT_H, COL_TF_FOC);
		nw_stroke_round(s, n->x, n->y, n->w, n->h, 6,
		                n->focused ? COL_TF_FOC : COL_TF_BRD, n->focused ? 255 : 200);
		break;
	}
	case NWUI_LIST: {
		nw_fill_round(s, n->x, n->y, n->w, n->h, 8, COL_TF_BG, 255);
		int maxs = n->count - n->h / NWUI_ROW_H;
		int has_sb = maxs > 0;
		int roww = n->w - 4 - (has_sb ? NWUI_SB_W : 0);    /* rows stop before the scrollbar */
		int vis = n->h / NWUI_ROW_H;
		for (int r = 0; r < vis; r++) {
			int idx = n->scroll + r;
			if (idx >= n->count) break;
			int ry = n->y + 2 + r * NWUI_ROW_H;
			int seld = (idx == n->sel);
			if (seld) nw_fill_round(s, n->x + 3, ry, roww, NWUI_ROW_H, 5, COL_SEL, 255);
			if (n->items && n->items[idx])
				nw_text(s, n->x + NWUI_TF_PAD + 3, ry + (NWUI_ROW_H - NW_FONT_H) / 2,
				        n->items[idx], seld ? 0x00ffffff : COL_INK);
		}
		nw_stroke_round(s, n->x, n->y, n->w, n->h, 8,
		                n->focused ? COL_TF_FOC : COL_TF_BRD, n->focused ? 255 : 200);
		if (has_sb) {                                      /* rounded scrollbar thumb */
			int sbx = n->x + n->w - NWUI_SB_W;
			int track = n->h - 6;
			int th = track * vis / n->count; if (th < NWUI_SB_MIN) th = NWUI_SB_MIN;
			if (th > track) th = track;
			int ty = n->y + 3 + (maxs > 0 ? (track - th) * n->scroll / maxs : 0);
			nw_fill_round(s, sbx + 2, ty, NWUI_SB_W - 5, th, (NWUI_SB_W - 5) / 2, COL_SB_THUMB, 255);
		}
		break;
	}
	case NWUI_ICONVIEW: {
		nw_fill_round(s, n->x, n->y, n->w, n->h, 8, COL_TF_BG, 255);
		int cols = n->cols < 1 ? 1 : n->cols;
		for (int i = 0; i < n->count; i++) {
			int row = i / cols, col = i % cols;
			int cx = n->x + col * NWUI_ICON_CELL_W;
			int cy = n->y + (row - n->scroll) * NWUI_ICON_CELL_H;
			if (cy < n->y || cy + NWUI_ICON_CELL_H > n->y + n->h) continue;  /* whole rows only */
			if (i == n->sel)        /* soft translucent rounded highlight (modern) */
				nw_fill_round(s, cx + 6, cy + 4, NWUI_ICON_CELL_W - 12, NWUI_ICON_CELL_H - 8, 12, COL_ACCENT, 32);
			const nwui_icon_item *it = &n->icons[i];
			if (it->icon && it->iw > 0 && it->ih > 0) {
				int iw = it->iw, ih = it->ih;
				int ix = cx + (NWUI_ICON_CELL_W - iw) / 2, iy = cy + 12;
				/* soft drop shadow under the icon for depth */
				nw_fill_round(s, ix + 5, iy + ih - 8, iw - 10, 12, 8, COL_ICON_SHADOW, 34);
				/* Alpha-composite the icon (0xAARRGGBB from the PNG decoder) over the cell, so
				 * anti-aliased edges + rounded corners blend cleanly and the selection shows
				 * through transparent areas. */
				for (int yy = 0; yy < ih; yy++)
					for (int xx = 0; xx < iw; xx++) {
						uint32_t p = it->icon[yy * iw + xx];
						int a = (int) (p >> 24);
						if (a) nw_blend_pixel(s, ix + xx, iy + yy, p & 0x00ffffff, a);
					}
			}
			if (it->label) {
				char buf[64];
				int len = 0;
				for (; it->label[len] && len < (int) sizeof buf - 1; len++)
					buf[len] = it->label[len];
				buf[len] = 0;
				/* truncate to the cell width by pixel measure, appending an ellipsis */
				if (nw_text_w(buf) > NWUI_ICON_CELL_W - 8) {
					while (len > 1 && nw_text_w(buf) > NWUI_ICON_CELL_W - 8) {
						buf[--len] = 0;
					}
					if (len > 1) { buf[len - 1] = '.'; if (len > 2) buf[len - 2] = '.'; }
				}
				int tx = cx + (NWUI_ICON_CELL_W - nw_text_w(buf)) / 2;
				int ty = cy + 12 + NWUI_ICON_PX + 6;
				nw_text(s, tx, ty, buf, (i == n->sel) ? COL_ACCENT_DEEP : COL_INK);
			}
		}
		nw_stroke_round(s, n->x, n->y, n->w, n->h, 8,
		                n->focused ? COL_TF_FOC : COL_TF_BRD, n->focused ? 255 : 200);
		break;
	}
	case NWUI_PANEL:
		nw_fill_round(s, n->x, n->y, n->w, n->h, 8, COL_PANEL_BG, 235);          /* glass body */
		nw_fill_round(s, n->x, n->y, n->w, NWUI_PANEL_TITLE_H, 8, COL_PANEL_HDR, 255); /* header */
		nw_text(s, n->x + 8, n->y + (NWUI_PANEL_TITLE_H - NW_FONT_H) / 2, n->text, 0x00ffffff);
		break;   /* children painted by the recursive walk */
	case NWUI_CHECKBOX: {
		int bs = 14, by = n->y + (n->h - bs) / 2;
		nw_fill_round(s, n->x, by, bs, bs, 3, COL_TF_BG, 255);
		nw_stroke_round(s, n->x, by, bs, bs, 3, n->focused ? COL_TF_FOC : COL_TF_BRD, 255);
		if (n->vbool && *n->vbool) {            /* a simple check mark from two strokes */
			nw_fill_rect(s, n->x + 3, by + 6, 3, 3, COL_SEL);
			nw_fill_rect(s, n->x + 6, by + 3, 3, 6, COL_SEL);
		}
		nw_text(s, n->x + bs + 6, n->y + (n->h - NW_FONT_H) / 2, n->text, COL_INK);
		break;
	}
	default:   /* row/column/box: paint own background if set (else transparent) */
		if (n->has_bg)
			nw_fill_round(s, n->x, n->y, n->w, n->h, 8, n->bg, 255);
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
	nw_fill_round(s, u->menu_x - 4, u->menu_y - 4, NWUI_MENU_W + 8, mh + 8, 9, 0x00f4f8fd, 255);
	nw_stroke_round(s, u->menu_x - 4, u->menu_y - 4, NWUI_MENU_W + 8, mh + 8, 9, 0x00b8c6d8, 220);
	for (int i = 0; i < NWUI_MI_COUNT; i++) {
		int iy = u->menu_y + i * NWUI_MENU_ITEM_H;
		int hov = (i == u->menu_hover);
		if (hov) nw_fill_round(s, u->menu_x - 1, iy, NWUI_MENU_W + 2, NWUI_MENU_ITEM_H, 5, COL_SEL, 255);
		nw_text(s, u->menu_x + 8, iy + (NWUI_MENU_ITEM_H - NW_FONT_H) / 2, L[i],
		        hov ? 0x00ffffff : COL_INK);
	}
}

int nwui_render(nwui *u, const struct nw_surface *s, int *x, int *y, int *w, int *h)
{
	if (!u->root)
		return 0;
	if (u->layout_dirty || u->modal) {   /* a modal always forces a full repaint so it stays on top */
		nwui_layout(u);
		nw_fill_rect(s, 0, 0, u->win_w, u->win_h, COL_WIN);
		paint_all(u->root, s);
		draw_menu(u, s);
		if (u->modal) {                                   /* dim backdrop + the modal on top */
			nw_blend_rect(s, 0, 0, u->win_w, u->win_h, 0x00000000, 90);
			nw_fill_round(s, u->modal->x - 8, u->modal->y - 8,
			              u->modal->w + 16, u->modal->h + 16, 10, 0x00f4f8fd, 255);
			nw_stroke_round(s, u->modal->x - 8, u->modal->y - 8,
			                u->modal->w + 16, u->modal->h + 16, 10, 0x00b8c6d8, 220);
			paint_all(u->modal, s);
		}
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
