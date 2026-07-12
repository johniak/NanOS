/*
 * nwui_paint.c — render the widget tree into a window surface via nw_gfx. Full repaint on a
 * layout change, otherwise only dirty nodes (-> a small damage rect the compositor blits).
 *
 * Two paint modes, gated on `u->glass` (set from NW_STYLE_GLASS_CLIENT at nwui_open_style()):
 *   - legacy (glass=0): opaque paper background, masked-RGB fills (nw_fill_, nw_blend_, nw_text
 *     family) — byte-identical to before this file gained glass support.
 *   - glass (glass=1): the window clears to a fully transparent ARGB canvas (the GL shader shows
 *     the desktop glass wherever alpha==0) and every widget paints STRAIGHT ARGB via the nw_over_
 *     nw_clear_argb, nw_text_argb family, because those are the only primitives that write the
 *     alpha byte — any masked-RGB call (nw_fill_rect, nw_fill_round, nw_stroke_round, nw_blend_
 *     family, nw_draw_char_t) always stores alpha=0, which would make that pixel fully transparent (i.e.
 *     invisible ink) under the glass shader. So in glass mode EVERY paint call in a widget's
 *     footprint must go through the ARGB path, not just the ones the design calls out with a new
 *     translucent colour.
 *
 * repaint_dirty() note: dirty repaints redraw only ONE node's footprint, not the whole window.
 * In glass mode the widget fills are translucent (nw_over_* src-over-composites onto whatever is
 * already there), so repainting the same dirty node twice without first restoring it to
 * GCOL_CANVAS would visibly darken/opacify a widget a little more on every repaint (blinking
 * caret, hover, keystrokes, ...). repaint_dirty() clears the node's rect to GCOL_CANVAS first in
 * glass mode so every dirty repaint starts from the same transparent base the full repaint would
 * have left it at — each widget already paints its own full footprint (the point of this file),
 * so nothing is lost by the clear.
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

/* ---- glass interiors (ARGB, straight alpha): two palettes, picked per window variant ----
 * Light slab (default glass): dark ink #17222f, whisper-white wells/pills.
 * Dark slab (NW_STYLE_DARK):  light ink #f2f6fb, the same well/pill family in white at lower
 * alphas (a dark slab needs less white to read as a surface). nwui_render() points `gp` at the
 * window's palette before any painting; painting is single-threaded per process. */
#define GCOL_CANVAS   0x00000000u   /* fully transparent: the GL slab shows through (both modes) */
struct gpal {
	uint32_t ink;        /* primary ink */
	uint32_t ink_soft;   /* secondary ink (muted labels) */
	uint32_t scrim;      /* text-pane scrim */
	uint32_t field;      /* input/list wells */
	uint32_t sel;        /* selection pill fill */
	uint32_t sel_ring;   /* selection inset hairline (brighter than the pill) */
	uint32_t btn_top, btn_bot, btn_ring;   /* button pill gradient + inset ring */
	uint32_t sep;        /* separators / idle field rings */
};
static const struct gpal PAL_LIGHT = {
	0xff17222fu, 0x9e17222fu, 0x40ffffffu, 0x30ffffffu, 0x59ffffffu, 0x80ffffffu,
	0x24ffffffu, 0x0dffffffu, 0x2effffffu, 0x2e17222fu,
};
static const struct gpal PAL_DARK = {
	0xfff2f6fbu, 0xa8dbe4f0u, 0x2effffffu, 0x26ffffffu, 0x3cffffffu, 0x6effffffu,
	0x1effffffu, 0x0affffffu, 0x28ffffffu, 0x30ffffffu,
};
static const struct gpal *gp = &PAL_LIGHT;
#define GCOL_INK      (gp->ink)
#define GCOL_INK_SOFT (gp->ink_soft)
#define GCOL_SCRIM    (gp->scrim)
#define GCOL_FIELD    (gp->field)
#define GCOL_SEL      (gp->sel)
#define GCOL_SEL_RING (gp->sel_ring)
#define GCOL_BTN_TOP  (gp->btn_top)
#define GCOL_BTN_BOT  (gp->btn_bot)
#define GCOL_BTN_RING (gp->btn_ring)
#define GCOL_SEP      (gp->sep)

/* Turn a legacy masked-RGB colour (0x00RRGGBB) + a 0..255 blend alpha into straight ARGB, for
 * spots the design table doesn't re-colour but that still need to carry real alpha in glass mode
 * (else the masked-RGB call they replace would silently stomp the pixel's alpha to 0). */
static inline uint32_t argb_op(uint32_t rgb, int a)
{
	return ((uint32_t) a << 24) | (rgb & 0x00ffffffu);
}

/* Primary/secondary ink for label-style text: unset fg -> ink; the toolkit's one muted-grey
 * constant -> the soft ink; any other app-chosen colour keeps its own RGB (made opaque ARGB). */
static uint32_t glass_ink(uint32_t fg)
{
	if (!fg) return GCOL_INK;
	if (fg == COL_MUTED) return GCOL_INK_SOFT;
	return argb_op(fg, 255);
}

/* Paint a 1px translucent border: nw_stroke_round has no straight-alpha counterpart, so a
 * translucent "stroke" is faked by filling the WHOLE rect in `ring`, then re-filling the interior
 * (inset 1px, radius-1) in `fill` — leaves exactly a 1px band of `ring` visible at the edge. Must
 * run BEFORE any content is painted over the fill (it repaints ~the whole footprint), unlike the
 * legacy nw_stroke_round which runs after content (a real outline only touches its own pixels). */
static void glass_ring(const struct nw_surface *s, int x, int y, int w, int h, int r,
                       uint32_t ring, uint32_t fill)
{
	/* One fill coat over the whole footprint, then the 1px AA rim as its own src-over band
	 * (nw_over_ring) — the rim can be BRIGHTER than the interior, like the mockup's inset
	 * hairline, and the interior alpha is exactly `fill`'s (no ring∘fill compounding). */
	nw_over_round(s, x, y, w, h, r, fill);
	nw_over_ring(s, x, y, w, h, r, ring);
}

static void paint_self(nwui_node *n, const struct nw_surface *s, int glass)
{
	switch (n->kind) {
	case NWUI_LABEL:
		if (glass) nw_text_argb(s, n->x, n->y, n->text, glass_ink(n->fg));
		else       nw_text(s, n->x, n->y, n->text, n->fg ? n->fg : COL_INK);
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
		if (n->flat && n->img) {               /* toolbar / icon-rail button */
			if (n->active) {                   /* current place in an icon rail: the selection pill */
				if (glass) glass_ring(s, n->x, n->y, n->w, n->h, 9, GCOL_SEL_RING, GCOL_SEL);
				else       nw_fill_round(s, n->x, n->y, n->w, n->h, 9, COL_ACCENT, 40);
			}
			if (n->pressed) {
				if (glass) nw_over_round(s, n->x, n->y + 1, n->w, n->h - 2, 7, argb_op(COL_ACCENT, 28));
				else       nw_fill_round(s, n->x, n->y + 1, n->w, n->h - 2, 7, COL_ACCENT, 28);
			}
			int iw = n->count, ih = n->sel;     /* native icon size (stashed by nwui_iconbtn) */
			int ix = n->x + (n->w - iw) / 2, iy = n->y + (n->h - ih) / 2;
			for (int yy = 0; yy < ih; yy++)
				for (int xx = 0; xx < iw; xx++) {
					uint32_t p = n->img[yy * iw + xx];
					int a = (int) (p >> 24);
					if (!a) continue;
					if (glass) nw_over_pixel(s, ix + xx, iy + yy, (uint32_t) (a << 24) | (p & 0x00ffffffu));
					else       nw_blend_pixel(s, ix + xx, iy + yy, p & 0x00ffffff, a);
				}
			break;
		}
		if (n->flat) {                         /* sidebar link / nav-row */
			int ty = n->y + (n->h - NW_FONT_H) / 2;
			if (n->active) {                   /* current location -> filled accent pill */
				if (glass) {
					glass_ring(s, n->x, n->y + 2, n->w, n->h - 4, 8, GCOL_SEL_RING, GCOL_SEL);
					nw_text_argb(s, n->x + 12, ty, n->text, GCOL_INK);
				} else {
					nw_fill_round(s, n->x, n->y + 2, n->w, n->h - 4, 8, COL_ACCENT, 255);
					nw_text(s, n->x + 12, ty, n->text, 0x00ffffff);
				}
			} else {
				if (n->pressed) {
					if (glass) nw_over_round(s, n->x, n->y + 2, n->w, n->h - 4, 8, argb_op(COL_ACCENT, 30));
					else       nw_fill_round(s, n->x, n->y + 2, n->w, n->h - 4, 8, COL_ACCENT, 30);
				}
				if (glass) nw_text_argb(s, n->x + 12, ty, n->text, glass_ink(n->fg));
				else       nw_text(s, n->x + 12, ty, n->text, n->fg ? n->fg : COL_INK);
			}
			break;
		}
		int down = n->pressed;
		if (glass) {
			if (n->has_bg) {
				nw_over_round(s, n->x, n->y, n->w, n->h, 7, argb_op(n->bg, 255));
			} else {
				uint32_t top = down ? 0x48ffffffu : GCOL_BTN_TOP;   /* pressed: alpha doubled */
				int ih = n->h - 2, ibh = ih / 2;
				nw_over_round(s, n->x, n->y, n->w, n->h, 7, GCOL_BTN_RING);           /* outer ring */
				nw_over_round(s, n->x + 1, n->y + 1, n->w - 2, ih, 6, top);           /* inset fill  */
				nw_over_rect(s, n->x + 1, n->y + 1 + ibh, n->w - 2, ih - ibh, GCOL_BTN_BOT); /* dark bottom */
			}
			int tx = n->x + (n->w - nw_text_w(n->text)) / 2;
			int ty = n->y + (n->h - NW_FONT_H) / 2;
			nw_text_argb(s, tx, ty, n->text, n->fg ? argb_op(n->fg, 255) : 0xffffffffu);
		} else {
			uint32_t base = n->has_bg ? n->bg : (down ? COL_BTN_DBOT : COL_BTN_BOT);
			nw_fill_round(s, n->x, n->y, n->w, n->h, 7, base, 255);        /* rounded solid fill */
			if (!n->has_bg)                                                /* glossy top sheen */
				nw_blend_rect(s, n->x + 3, n->y + 2, n->w - 6, (n->h - 4) / 2,
				              down ? COL_BTN_DTOP : COL_BTN_TOP, 120);
			nw_stroke_round(s, n->x, n->y, n->w, n->h, 7, 0x00ffffff, 60);
			int tx = n->x + (n->w - nw_text_w(n->text)) / 2;
			int ty = n->y + (n->h - NW_FONT_H) / 2;
			nw_text(s, tx, ty, n->text, n->fg ? n->fg : 0x00ffffff);
		}
		break;
	}
	case NWUI_TEXTFIELD: {
		if (glass) {
			/* glass_ring paints the well: one GCOL_FIELD coat + the focus/sep ring as a real
			 * 1px band (nw_over_ring), so the ring colour never tints the interior. */
			uint32_t ring = n->focused ? argb_op(COL_TF_FOC, 90) : GCOL_SEP;
			glass_ring(s, n->x, n->y, n->w, n->h, 9, ring, GCOL_FIELD);
		} else {
			nw_fill_round(s, n->x, n->y, n->w, n->h, 6, COL_TF_BG, 255);
			nw_stroke_round(s, n->x, n->y, n->w, n->h, 6, n->focused ? COL_TF_FOC : COL_TF_BRD,
			                n->focused ? 255 : 200);
			if (n->focused)                                     /* a second ring = a soft 2px focus */
				nw_stroke_round(s, n->x + 1, n->y + 1, n->w - 2, n->h - 2, 5, COL_TF_FOC, 120);
		}
		int tx = n->x + NWUI_TF_PAD, ty = n->y + (n->h - NW_FONT_H) / 2;
		/* placeholder: an empty, unfocused field shows its caption text (n->text — unused for
		 * anything else on a textfield) in the muted ink, like a search field's "Search". */
		if (n->tlen == 0 && !n->focused && n->text[0]) {
			if (glass) nw_text_argb(s, tx + 2, ty, n->text, GCOL_INK_SOFT);
			else       nw_text(s, tx + 2, ty, n->text, COL_MUTED);
		}
		int lo = n->anchor < n->caret ? n->anchor : n->caret;
		int hi = n->anchor > n->caret ? n->anchor : n->caret;
		for (int i = 0; i < n->tlen; i++) {
			int sel = (n->anchor != n->caret && i >= lo && i < hi);
			int cx = tx + i * NW_FONT_W;
			unsigned char glyph = n->secret ? (unsigned char) '*' : (unsigned char) n->tbuf[i];
			if (glass) {
				if (sel) nw_over_rect(s, cx, ty, NW_FONT_W, NW_FONT_H, GCOL_SEL);
				char cbuf[2] = { (char) glyph, 0 };
				nw_text_argb(s, cx, ty, cbuf, GCOL_INK);
			} else {
				if (sel) nw_fill_rect(s, cx, ty, NW_FONT_W, NW_FONT_H, COL_SEL);
				nw_draw_char_t(s, cx, ty, glyph, sel ? 0x00ffffff : COL_INK);
			}
		}
		if (n->focused) {
			if (glass) nw_over_rect(s, tx + n->caret * NW_FONT_W, ty, 2, NW_FONT_H, argb_op(COL_TF_FOC, 255));
			else       nw_fill_rect(s, tx + n->caret * NW_FONT_W, ty, 2, NW_FONT_H, COL_TF_FOC);
		}
		break;
	}
	case NWUI_TEXTAREA: {
		if (glass) {
			/* single well fill: see the NWUI_TEXTFIELD comment above — glass_ring's own inset
			 * fill IS the well's fill, don't paint GCOL_FIELD again before calling it. */
			uint32_t ring = n->focused ? argb_op(COL_TF_FOC, 90) : GCOL_SEP;
			glass_ring(s, n->x, n->y, n->w, n->h, 9, ring, GCOL_FIELD);
		} else {
			nw_fill_round(s, n->x, n->y, n->w, n->h, 6, COL_TF_BG, 255);
		}
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
						if (glass) {
							if (seld) nw_over_rect(s, xx, yy, NW_FONT_W, NW_FONT_H, GCOL_SEL);
							char cbuf[2] = { n->tbuf[i], 0 };
							nw_text_argb(s, xx, yy, cbuf, GCOL_INK);
						} else {
							if (seld) nw_fill_rect(s, xx, yy, NW_FONT_W, NW_FONT_H, COL_SEL);
							nw_draw_char_t(s, xx, yy, (unsigned char) n->tbuf[i],
							               seld ? 0x00ffffff : COL_INK);
						}
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
		if (caret_px >= 0) {
			if (glass) nw_over_rect(s, caret_px, caret_py, 2, NW_FONT_H, argb_op(COL_TF_FOC, 255));
			else       nw_fill_rect(s, caret_px, caret_py, 2, NW_FONT_H, COL_TF_FOC);
		}
		if (!glass)
			nw_stroke_round(s, n->x, n->y, n->w, n->h, 6,
			                n->focused ? COL_TF_FOC : COL_TF_BRD, n->focused ? 255 : 200);
		break;
	}
	case NWUI_LIST: {
		if (glass) {
			/* single well fill: see the NWUI_TEXTFIELD comment above. */
			uint32_t ring = n->focused ? argb_op(COL_TF_FOC, 90) : GCOL_SEP;
			glass_ring(s, n->x, n->y, n->w, n->h, 12, ring, GCOL_FIELD);
		} else {
			nw_fill_round(s, n->x, n->y, n->w, n->h, 8, COL_TF_BG, 255);
		}
		int maxs = n->count - n->h / NWUI_ROW_H;
		int has_sb = maxs > 0;
		int roww = n->w - 4 - (has_sb ? NWUI_SB_W : 0);    /* rows stop before the scrollbar */
		int vis = n->h / NWUI_ROW_H;
		for (int r = 0; r < vis; r++) {
			int idx = n->scroll + r;
			if (idx >= n->count) break;
			int ry = n->y + 2 + r * NWUI_ROW_H;
			int seld = (idx == n->sel);
			if (glass) {
				if (seld) glass_ring(s, n->x + 3, ry, roww, NWUI_ROW_H, 5, GCOL_SEL_RING, GCOL_SEL);
				if (n->items && n->items[idx])
					nw_text_argb(s, n->x + NWUI_TF_PAD + 3, ry + (NWUI_ROW_H - NW_FONT_H) / 2,
					             n->items[idx], GCOL_INK);
			} else {
				if (seld) nw_fill_round(s, n->x + 3, ry, roww, NWUI_ROW_H, 5, COL_SEL, 255);
				if (n->items && n->items[idx])
					nw_text(s, n->x + NWUI_TF_PAD + 3, ry + (NWUI_ROW_H - NW_FONT_H) / 2,
					        n->items[idx], seld ? 0x00ffffff : COL_INK);
			}
		}
		if (!glass)
			nw_stroke_round(s, n->x, n->y, n->w, n->h, 8,
			                n->focused ? COL_TF_FOC : COL_TF_BRD, n->focused ? 255 : 200);
		if (has_sb) {                                      /* rounded scrollbar thumb */
			int sbx = n->x + n->w - NWUI_SB_W;
			int track = n->h - 6;
			int th = track * vis / n->count; if (th < NWUI_SB_MIN) th = NWUI_SB_MIN;
			if (th > track) th = track;
			int ty = n->y + 3 + (maxs > 0 ? (track - th) * n->scroll / maxs : 0);
			if (glass) nw_over_round(s, sbx + 2, ty, NWUI_SB_W - 5, th, (NWUI_SB_W - 5) / 2, argb_op(COL_SB_THUMB, 255));
			else       nw_fill_round(s, sbx + 2, ty, NWUI_SB_W - 5, th, (NWUI_SB_W - 5) / 2, COL_SB_THUMB, 255);
		}
		break;
	}
	case NWUI_ICONVIEW: {
		if (glass) {
			/* single well fill: see the NWUI_TEXTFIELD comment above. Dark windows draw NO well
			 * — the grid sits seamlessly on the slab (the Files-redesign screenshot language). */
			uint32_t ring = n->focused ? argb_op(COL_TF_FOC, 90) : GCOL_SEP;
			if (!n->owner->dark)
				glass_ring(s, n->x, n->y, n->w, n->h, 12, ring, GCOL_FIELD);
		} else {
			nw_fill_round(s, n->x, n->y, n->w, n->h, 8, COL_TF_BG, 255);
		}
		int cols = n->cols < 1 ? 1 : n->cols;
		int rows = (n->count + cols - 1) / cols;
		int content_h = rows * NWUI_ICON_CELL_H;
		int has_sb = content_h > n->h;
		int cw = n->w - (has_sb ? NWUI_SB_W : 0);
		/* Clip cell drawing to the content area so PARTIAL rows scroll smoothly under the top/bottom
		 * edges (the scroll offset is in pixels). Restored to no-scissor after the cells. */
		struct nw_surface *ms = (struct nw_surface *) s;
		nw_surface_clip(ms, n->x, n->y, cw, n->h);
		for (int i = 0; i < n->count; i++) {
			int row = i / cols, col = i % cols;
			int cx = n->x + col * NWUI_ICON_CELL_W;
			int cy = n->y + row * NWUI_ICON_CELL_H - n->scroll;
			if (cy + NWUI_ICON_CELL_H <= n->y || cy >= n->y + n->h) continue;  /* fully off-view */
			int seld = n->selmask ? n->selmask[i] : (i == n->sel);
			int sx = cx + 6, sy = cy + 4, sw = NWUI_ICON_CELL_W - 12, sh = NWUI_ICON_CELL_H - 8;
			if (glass) {
				/* The lead cell (i == n->sel) gets its GCOL_SEL fill from glass_ring's own
				 * single inset fill below, so only paint the plain direct fill here for a
				 * SELECTED-BUT-NOT-LEAD cell (multi-select) — else the lead cell would receive
				 * GCOL_SEL twice (this direct fill, then glass_ring's fill again). */
				if (seld && i != n->sel)   /* soft translucent rounded highlight (modern) */
					nw_over_round(s, sx, sy, sw, sh, 12, GCOL_SEL);
				if (i == n->sel)        /* the LEAD cell: ring + its own single fill */
					glass_ring(s, sx, sy, sw, sh, 12, GCOL_SEL_RING, seld ? GCOL_SEL : GCOL_CANVAS);
				if (i == n->drop_hover) /* drop target: filled + outlined, kept in the accent colour */
					glass_ring(s, sx - 1, sy - 1, sw + 2, sh + 2, 12,
					          argb_op(COL_ACCENT_DEEP, 220), argb_op(COL_ACCENT, 64));
			} else {
				if (seld)
					nw_fill_round(s, sx, sy, sw, sh, 12, COL_ACCENT, 32);
				if (i == n->sel)
					nw_stroke_round(s, sx, sy, sw, sh, 12, COL_ACCENT_DEEP, 110);
				if (i == n->drop_hover) {
					nw_fill_round(s, sx, sy, sw, sh, 12, COL_ACCENT, 64);
					nw_stroke_round(s, sx - 1, sy - 1, sw + 2, sh + 2, 12, COL_ACCENT_DEEP, 220);
				}
			}
			const nwui_icon_item *it = &n->icons[i];
			if (it->icon && it->iw > 0 && it->ih > 0) {
				int iw = it->iw, ih = it->ih;
				int ix = cx + (NWUI_ICON_CELL_W - iw) / 2, iy = cy + 12;
				/* soft drop shadow under the icon for depth */
				if (glass) nw_over_round(s, ix + 5, iy + ih - 8, iw - 10, 12, 8, 0x22000000u);
				else       nw_fill_round(s, ix + 5, iy + ih - 8, iw - 10, 12, 8, COL_ICON_SHADOW, 34);
				/* Alpha-composite the icon (0xAARRGGBB from the PNG decoder) over the cell, so
				 * anti-aliased edges + rounded corners blend cleanly and the selection shows
				 * through transparent areas. */
				for (int yy = 0; yy < ih; yy++)
					for (int xx = 0; xx < iw; xx++) {
						uint32_t p = it->icon[yy * iw + xx];
						int a = (int) (p >> 24);
						if (!a) continue;
						if (glass) nw_over_pixel(s, ix + xx, iy + yy, (uint32_t) (a << 24) | (p & 0x00ffffffu));
						else       nw_blend_pixel(s, ix + xx, iy + yy, p & 0x00ffffff, a);
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
				if (glass) {
					/* raw glass caption: a cheap 4-neighbour soft halo drawn first so the ink
					 * reads over any backdrop, then the core ink on top. */
					static const int hdx[4] = { -1, 1, 0, 0 };
					static const int hdy[4] = { 0, 0, -1, 1 };
					for (int k = 0; k < 4; k++)
						nw_text_argb(s, tx + hdx[k], ty + hdy[k], buf, 0x66f2f6fau);
					nw_text_argb(s, tx, ty, buf, GCOL_INK);
				} else {
					nw_text(s, tx, ty, buf, seld ? COL_ACCENT_DEEP : COL_INK);
				}
			}
		}
		nw_surface_noclip(ms);
		/* vertical scrollbar (thumb sized + positioned in PIXELS) when the grid overflows */
		if (has_sb) {
			int sbx = n->x + n->w - NWUI_SB_W;
			int track = n->h - 6;
			int th = track * n->h / content_h; if (th < NWUI_SB_MIN) th = NWUI_SB_MIN;
			if (th > track) th = track;
			int maxs = content_h - n->h;
			int ty = n->y + 3 + (track - th) * n->scroll / (maxs > 0 ? maxs : 1);
			if (glass) nw_over_round(s, sbx + 2, ty, NWUI_SB_W - 5, th, (NWUI_SB_W - 5) / 2, argb_op(COL_SB_THUMB, 255));
			else       nw_fill_round(s, sbx + 2, ty, NWUI_SB_W - 5, th, (NWUI_SB_W - 5) / 2, COL_SB_THUMB, 255);
		}
		if (!glass)
			nw_stroke_round(s, n->x, n->y, n->w, n->h, 8,
			                n->focused ? COL_TF_FOC : COL_TF_BRD, n->focused ? 255 : 200);
		break;
	}
	case NWUI_PANEL:
		if (glass) {
			/* body melts into the surrounding glass (same feather rule as has_bg containers
			 * below); the title header stays a crisp band on top of it. */
			int pf = (n->w < n->h ? n->w : n->h) / 8;
			if (pf > 14) pf = 14;
			nw_over_round_soft(s, n->x, n->y, n->w, n->h, 14, GCOL_SCRIM, pf);       /* glass body */
			nw_over_round(s, n->x, n->y, n->w, NWUI_PANEL_TITLE_H, 8,
			             (uint32_t) (0x66u << 24) | (COL_PANEL_HDR & 0x00ffffffu));  /* header */
			nw_text_argb(s, n->x + 8, n->y + (NWUI_PANEL_TITLE_H - NW_FONT_H) / 2, n->text, 0xffffffffu);
		} else {
			nw_fill_round(s, n->x, n->y, n->w, n->h, 8, COL_PANEL_BG, 235);          /* glass body */
			nw_fill_round(s, n->x, n->y, n->w, NWUI_PANEL_TITLE_H, 8, COL_PANEL_HDR, 255); /* header */
			nw_text(s, n->x + 8, n->y + (NWUI_PANEL_TITLE_H - NW_FONT_H) / 2, n->text, 0x00ffffff);
		}
		break;   /* children painted by the recursive walk */
	case NWUI_CHECKBOX: {
		int bs = 14, by = n->y + (n->h - bs) / 2;
		if (glass) {
			/* single well fill: see the NWUI_TEXTFIELD comment above. */
			uint32_t ring = n->focused ? argb_op(COL_TF_FOC, 90) : GCOL_SEP;
			glass_ring(s, n->x, by, bs, bs, 3, ring, GCOL_FIELD);
			if (n->vbool && *n->vbool) {
				nw_over_rect(s, n->x + 3, by + 6, 3, 3, argb_op(COL_SEL, 255));
				nw_over_rect(s, n->x + 6, by + 3, 3, 6, argb_op(COL_SEL, 255));
			}
			nw_text_argb(s, n->x + bs + 6, n->y + (n->h - NW_FONT_H) / 2, n->text, glass_ink(n->fg));
		} else {
			nw_fill_round(s, n->x, by, bs, bs, 3, COL_TF_BG, 255);
			nw_stroke_round(s, n->x, by, bs, bs, 3, n->focused ? COL_TF_FOC : COL_TF_BRD, 255);
			if (n->vbool && *n->vbool) {            /* a simple check mark from two strokes */
				nw_fill_rect(s, n->x + 3, by + 6, 3, 3, COL_SEL);
				nw_fill_rect(s, n->x + 6, by + 3, 3, 6, COL_SEL);
			}
			nw_text(s, n->x + bs + 6, n->y + (n->h - NW_FONT_H) / 2, n->text, COL_INK);
		}
		break;
	}
	default:   /* row/column/box: paint own background if set (else transparent) */
		if (n->has_bg) {
			if (glass) {
				/* A legacy 0RGB colour (top byte 0 — an app's plain .colors() call, authored
				 * pre-glass) forcing alpha 255 here made every such container an opaque panel,
				 * breaking the glass slab's continuity (e.g. Files' sidebar, Notepad's status
				 * bar) and creating a visible seam against the translucent siblings around it.
				 * Composite it as a translucent scrim instead (0x59, a moderate general-purpose
				 * panel alpha in the same family as GCOL_SCRIM/GCOL_SEL_RING). An app that set
				 * its OWN top byte is making an explicit alpha choice — honour it verbatim.
				 * Feathered fill: a passive panel is a region of the slab, not a control, so
				 * instead of ending on a hard line its scrim melts into the surrounding glass
				 * over ~1/8 of its smaller side (capped — big sidebars shouldn't fade forever). */
				uint32_t bg = n->bg;
				int f = (n->w < n->h ? n->w : n->h) / 8;
				if (f > 14) f = 14;
				nw_over_round_soft(s, n->x, n->y, n->w, n->h, 14,
				                   (bg >> 24) ? bg : argb_op(bg, 0x40), f);
			} else {
				nw_fill_round(s, n->x, n->y, n->w, n->h, 8, n->bg, 255);
			}
		}
		break;
	}
}

/* pre-order so parents paint before children (children on top) */
static void paint_all(nwui_node *n, const struct nw_surface *s, int glass)
{
	if (n->hidden) return;                 /* hidden subtree: don't paint it or its children */
	paint_self(n, s, glass);
	for (int i = 0; i < n->nchild; i++)
		paint_all(n->child[i], s, glass);
}
static void clear_dirty(nwui_node *n)
{
	n->dirty = 0;
	for (int i = 0; i < n->nchild; i++)
		clear_dirty(n->child[i]);
}

struct dmg { int have, x0, y0, x1, y1; };

/* Ancestor stack depth for the glass-mode dirty-repaint recompose below: nodes have no parent
 * back-pointer (nwui_node has only child[]/nchild), so repaint_dirty threads the root-to-here
 * ancestor chain DOWN through the recursion instead of walking up from a dirty leaf. NWUI trees
 * in this toolkit are shallow (a handful of box/row/column/panel levels); 32 is a generous cap. */
#define NWUI_PAINT_MAX_DEPTH 32

static void repaint_dirty(nwui_node *n, const struct nw_surface *s, struct dmg *d, int glass,
                          nwui_node *const *anc, int nanc)
{
	if (n->hidden) return;                 /* hidden subtree: skip */
	if (n->dirty) {
		if (glass) {
			/* Glass mode: translucent fills src-over-composite, so repainting this node's OLD
			 * pixels again would accumulate alpha (a hovered button/blinking caret/typed field
			 * would darken a little more on every repaint). Reset the footprint to the
			 * transparent canvas first, matching what a full repaint's window-wide clear does.
			 *
			 * But dirty flags land on LEAF nodes only (buttons, textfields, ... — see
			 * nwui_core.c); a leaf nested inside an ancestor that paints its own background
			 * (e.g. an NWUI_PANEL scrim) needs that ancestor's contribution recomposited under
			 * it too, or the clear above wipes the ancestor's scrim from this one patch and it
			 * ends up permanently more transparent than the rest of the panel. Recompose every
			 * ancestor (root-downward) that owns a background, each clipped to THIS node's rect
			 * via the surface scissor so only the dirty footprint is touched — mirrors exactly
			 * what the full-repaint tree walk would have painted there (paint_self only paints
			 * a container's own background, never recurses into children, so this can't
			 * duplicate sibling content). */
			nw_clear_argb(s, n->x, n->y, n->w, n->h, GCOL_CANVAS);
			struct nw_surface *ms = (struct nw_surface *) s;
			nw_surface_clip(ms, n->x, n->y, n->w, n->h);
			for (int i = 0; i < nanc; i++)
				paint_self(anc[i], s, glass);
			nw_surface_noclip(ms);
			/* paint_all rather than paint_self: dirty is leaf-only today, but if a node with
			 * children ever gets marked dirty directly, its own subtree still needs painting —
			 * paint_all does exactly that (paint_self then recurse), a no-op walk for leaves. */
			paint_all(n, s, glass);
		} else {
			paint_self(n, s, glass);
		}
		if (!d->have) { d->x0 = n->x; d->y0 = n->y; d->x1 = n->x + n->w; d->y1 = n->y + n->h; d->have = 1; }
		else {
			if (n->x < d->x0) d->x0 = n->x;
			if (n->y < d->y0) d->y0 = n->y;
			if (n->x + n->w > d->x1) d->x1 = n->x + n->w;
			if (n->y + n->h > d->y1) d->y1 = n->y + n->h;
		}
		n->dirty = 0;
	}
	if (n->nchild > 0) {
		nwui_node *anc2[NWUI_PAINT_MAX_DEPTH];
		int nanc2 = nanc;
		for (int i = 0; i < nanc && i < NWUI_PAINT_MAX_DEPTH; i++) anc2[i] = anc[i];
		if (nanc < NWUI_PAINT_MAX_DEPTH) anc2[nanc2++] = n;
		for (int i = 0; i < n->nchild; i++)
			repaint_dirty(n->child[i], s, d, glass, anc2, nanc2);
	}
}

/* the context-menu overlay, drawn last (on top of everything). Always part of a full repaint
 * (opening/closing a menu forces layout_dirty), so — unlike the widgets above — it never needs
 * the dirty-repaint accumulation guard. */
static void draw_menu(const nwui *u, const struct nw_surface *s)
{
	if (!u->menu_open)
		return;
	static const char *const BUILTIN[NWUI_MI_COUNT] = { "Cut", "Copy", "Paste", "Select All" };
	int count = u->menu_custom ? u->cmenu_n : NWUI_MI_COUNT;
	int mh = count * NWUI_MENU_ITEM_H;
	if (u->glass) {
		/* floating surface: kept NEARLY opaque on purpose (text-dense floats need a strong scrim
		 * to read over arbitrary desktop content), not the light interior translucency above.
		 * Dark windows float a dark sheet so the palette's light ink stays readable. */
		glass_ring(s, u->menu_x - 4, u->menu_y - 4, NWUI_MENU_W + 8, mh + 8, 9,
		          u->dark ? argb_op(0x00081019, 220) : argb_op(0x00b8c6d8, 220),
		          u->dark ? 0xf01b2534u : 0xf0f7fafdu);
	} else {
		nw_fill_round(s, u->menu_x - 4, u->menu_y - 4, NWUI_MENU_W + 8, mh + 8, 9, 0x00f4f8fd, 255);
		nw_stroke_round(s, u->menu_x - 4, u->menu_y - 4, NWUI_MENU_W + 8, mh + 8, 9, 0x00b8c6d8, 220);
	}
	for (int i = 0; i < count; i++) {
		const char *lbl = u->menu_custom ? u->cmenu_label[i] : BUILTIN[i];
		int iy = u->menu_y + i * NWUI_MENU_ITEM_H;
		int hov = (i == u->menu_hover);
		if (u->glass) {
			if (hov) nw_over_round(s, u->menu_x - 1, iy, NWUI_MENU_W + 2, NWUI_MENU_ITEM_H, 5, argb_op(COL_SEL, 255));
			nw_text_argb(s, u->menu_x + 8, iy + (NWUI_MENU_ITEM_H - NW_FONT_H) / 2, lbl ? lbl : "",
			            hov ? 0xffffffffu : GCOL_INK);
		} else {
			if (hov) nw_fill_round(s, u->menu_x - 1, iy, NWUI_MENU_W + 2, NWUI_MENU_ITEM_H, 5, COL_SEL, 255);
			nw_text(s, u->menu_x + 8, iy + (NWUI_MENU_ITEM_H - NW_FONT_H) / 2, lbl ? lbl : "",
			        hov ? 0x00ffffff : COL_INK);
		}
	}
}

int nwui_render(nwui *u, const struct nw_surface *s, int *x, int *y, int *w, int *h)
{
	if (!u->root)
		return 0;
	gp = u->dark ? &PAL_DARK : &PAL_LIGHT;   /* the window's glass palette, for every paint below */
	if (u->layout_dirty || u->modal) {   /* a modal always forces a full repaint so it stays on top */
		nwui_layout(u);
		if (u->glass) nw_clear_argb(s, 0, 0, u->win_w, u->win_h, GCOL_CANVAS);
		else          nw_fill_rect(s, 0, 0, u->win_w, u->win_h, COL_WIN);
		paint_all(u->root, s, u->glass);
		draw_menu(u, s);
		if (u->modal) {                                   /* dim backdrop + the modal on top */
			if (u->glass) {
				nw_over_rect(s, 0, 0, u->win_w, u->win_h, 0x5a000000u);
				glass_ring(s, u->modal->x - 8, u->modal->y - 8, u->modal->w + 16, u->modal->h + 16, 10,
				          u->dark ? argb_op(0x00081019, 220) : argb_op(0x00b8c6d8, 220),
				          u->dark ? 0xf01b2534u : 0xf0f7fafdu);
			} else {
				nw_blend_rect(s, 0, 0, u->win_w, u->win_h, 0x00000000, 90);
				nw_fill_round(s, u->modal->x - 8, u->modal->y - 8,
				              u->modal->w + 16, u->modal->h + 16, 10, 0x00f4f8fd, 255);
				nw_stroke_round(s, u->modal->x - 8, u->modal->y - 8,
				                u->modal->w + 16, u->modal->h + 16, 10, 0x00b8c6d8, 220);
			}
			paint_all(u->modal, s, u->glass);
		}
		clear_dirty(u->root);
		*x = 0; *y = 0; *w = u->win_w; *h = u->win_h;
		return 1;
	}
	struct dmg d = { 0, 0, 0, 0, 0 };
	repaint_dirty(u->root, s, &d, u->glass, 0, 0);
	if (!d.have)
		return 0;
	*x = d.x0; *y = d.y0; *w = d.x1 - d.x0; *h = d.y1 - d.y0;
	return 1;
}
