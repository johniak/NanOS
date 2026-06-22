/*
 * nw_compose.c — modern software compositor: gradient wallpaper, rounded translucent "glass"
 * windows with soft shadows, a macOS-style top bar and a bottom dock. Pure (no I/O): renders
 * the server's window list into a backbuffer the shell blits to /dev/fb0.
 *
 * Each window is drawn opaquely into a screen-sized `scratch` buffer (chrome + content), then
 * composited onto the scene with anti-aliased rounded corners and per-window alpha, reading
 * the already-composited backdrop for the translucency. The wallpaper is pre-rendered once.
 */
#include "nw_compose.h"

/* ---- palette (0x00RRGGBB) -------------------------------------------------------- */
#define COL_WIN_LIGHT  0xf8fbff   /* light window material           */
#define COL_WIN_DARK   0x0f121f   /* terminal-style dark window      */
#define COL_TB_TOP     0xffffff   /* light title bar gradient        */
#define COL_TB_BOT     0xe6edf8
#define COL_TB_DTOP    0x262c3e   /* dark title bar gradient         */
#define COL_TB_DBOT    0x141928
#define COL_TITLE_FG   0x1a2638
#define COL_TITLE_DFG  0xdfe9fb
#define COL_BORDER     0x9fb2cc   /* hairline window border          */
#define COL_CTRL       0x44516a   /* window control glyphs           */
#define COL_CTRL_D     0xc6d2e6
#define COL_CLOSE_HOV  0xe81123
#define WIN_ALPHA      234        /* translucent "glass" body over the pre-blurred backdrop */
#define DARK_ALPHA     236

#define COL_PANEL      0xeef7ff   /* top bar tint (translucent)       */
#define COL_PANEL_FG   0x141d2e
#define COL_PANEL_MUT  0x5d6b80
#define COL_DOCK       0xeef8ff
#define COL_CURSOR_FG  0x101620
#define COL_CURSOR_BG  0xffffff

/* Classic 11x16 arrow cursor: 'X' outline, '.' fill, ' ' transparent. */
static const char *const CURSOR[16] = {
	"X          ", "XX         ", "X.X        ", "X..X       ",
	"X...X      ", "X....X     ", "X.....X    ", "X......X   ",
	"X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ",
	"X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     XXX   "
};

/* ---- wallpaper (rendered once) --------------------------------------------------- */
/* Colourful radial blobs over a soft diagonal gradient — the NanoOS desktop. */
void nw_render_wallpaper(const struct nw_surface *dst)
{
	int W = dst->w, H = dst->h;
	struct blob { int cx, cy, r; uint32_t col; } b[4] = {
		{ W * 14 / 100, H * 23 / 100, W * 30 / 100, 0x12a8f4 },   /* blue   */
		{ W * 78 / 100, H * 28 / 100, W * 30 / 100, 0xff9d00 },   /* orange */
		{ W * 60 / 100, H * 78 / 100, W * 28 / 100, 0x6fd033 },   /* green  */
		{ W * 18 / 100, H * 84 / 100, W * 32 / 100, 0x7d3ff2 },   /* purple */
	};
	for (int y = 0; y < H; y++) {
		uint32_t base = nw_lerp(0xe3f7ff, 0xfff0d8, y, H > 1 ? H - 1 : 1);
		uint32_t *row = dst->px + (long) y * dst->stride;
		for (int x = 0; x < W; x++) {
			uint32_t c = base;
			for (int k = 0; k < 4; k++) {
				int dx = x - b[k].cx, dy = y - b[k].cy;
				int d2 = dx * dx + dy * dy, r2 = b[k].r * b[k].r;
				if (d2 < r2) {
					/* soft falloff: strongest at centre, 0 at the edge */
					int t = (int) (__builtin_sqrtf((float) d2) * 255.0f / b[k].r);
					int a = (255 - t) * 168 / 255;          /* peak ~0.66 */
					c = nw_mix(c, b[k].col, a);
				}
			}
			row[x] = c;
		}
	}
}

/* ---- window rendering ------------------------------------------------------------ */
static int frame_w(const struct nw_window *w) { return w->cw + 2 * NW_BORDER; }
static int frame_h(const struct nw_window *w) { return NW_TITLEBAR_H + w->ch + NW_BORDER; }

/* Render the window opaquely (material + title bar + controls + content) into `sc`, with the
 * frame's top-left at (ox,oy). Callers pass the on-screen (w->x,w->y) to draw into a screen-space
 * buffer, or (0,0) to render window-local into the per-window frame cache. The rounded/alpha
 * composite happens afterwards. */
static void draw_window_to(const struct nw_surface *sc, const struct nw_window *w, int focused,
                           int ox, int oy)
{
	int fw = frame_w(w), fh = frame_h(w);
	int dark = (w->title[0] == '\x01');       /* a leading 0x01 in the title flags a dark window */
	uint32_t mat = dark ? COL_WIN_DARK : COL_WIN_LIGHT;
	const char *title = dark ? w->title + 1 : w->title;

	nw_fill_rect(sc, ox, oy, fw, fh, mat);                          /* material */
	nw_vgrad_rect(sc, ox, oy, fw, NW_TITLEBAR_H,                    /* title bar */
	              dark ? COL_TB_DTOP : COL_TB_TOP, dark ? COL_TB_DBOT : COL_TB_BOT);
	if (!focused)                                                    /* dim the bar when unfocused */
		nw_blend_rect(sc, ox, oy, fw, NW_TITLEBAR_H, mat, 80);

	/* title text (with a little app dot to the left) */
	uint32_t tfg = dark ? COL_TITLE_DFG : COL_TITLE_FG;
	int ty = oy + (NW_TITLEBAR_H - NW_FONT_H) / 2;
	nw_fill_round(sc, ox + 10, ty + 2, 12, 12, 3, focused ? 0x12a8f4 : 0x9fb2cc, 255);
	nw_text(sc, ox + 28, ty, title, tfg);                 /* bg 0 = ignored (opaque text bg) */

	/* window controls on the right: —  □  × */
	uint32_t cfg = dark ? COL_CTRL_D : COL_CTRL;
	int cw = NW_CLOSE, ch2 = NW_TITLEBAR_H;
	int x3 = ox + fw - cw;                         /* × */
	int x2 = x3 - cw;                              /* □ */
	int x1 = x2 - cw;                              /* — */
	int gy = oy + ch2 / 2;
	nw_blend_rect(sc, x1 + cw / 2 - 4, gy, 8, 2, cfg, 255);                       /* minimize */
	nw_stroke_round(sc, x2 + cw / 2 - 5, gy - 5, 10, 10, 2, cfg, 255);            /* maximize */
	for (int i = -4; i <= 4; i++) {                                               /* close × */
		nw_blend_pixel(sc, x3 + cw / 2 + i, gy - 4 + (i + 4), cfg, 255);
		nw_blend_pixel(sc, x3 + cw / 2 + i, gy + 4 - (i + 4), cfg, 255);
	}

	/* content */
	int cox = ox + NW_BORDER, coy = oy + NW_TITLEBAR_H;
	if (w->buf) {
		struct nw_surface src;
		src.px = w->buf; src.w = w->cw; src.h = w->ch; src.stride = w->cw;
		nw_surface_noclip(&src);
		nw_blit(sc, cox, coy, &src, 0, 0, w->cw, w->ch);
	} else {
		nw_fill_rect(sc, cox, coy, w->cw, w->ch, mat);
	}
}

/* src over dst at coverage a. Uses the shared RB-paired blend (nw_gfx.h, inlined), so the
 * per-pixel hot loop has no cross-TU call and the compositor and rasterizer blend identically. */
static inline uint32_t cmix(uint32_t d, uint32_t s, int a)
{
	return nw_blend8(d, s, (unsigned) a);
}

/* Composite the source window onto `back` with rounded corners (AA) + per-window alpha. The
 * source pixel for back(px,py) is sc[(py-soy)*stride + (px-sox)]; pass (sox,soy)=(x,y) when the
 * source is a window-local frame (origin at the window's top-left), or (0,0) when it is a
 * screen-space buffer aligned with `back`. Fast: clip bounds resolved once; straight middle rows
 * are one tight blend loop (or memcpy when opaque); only the two corner bands pay per-pixel AA. */
static void composite_round(const struct nw_surface *back, const struct nw_surface *sc,
                            int x, int y, int w, int h, int r, int alpha, int sox, int soy,
                            const struct nw_surface *backdrop)
{
	int bx0, by0, bx1, by1;
	nw_surface_bounds(back, &bx0, &by0, &bx1, &by1);
	int x0 = x < bx0 ? bx0 : x, x1 = x + w > bx1 ? bx1 : x + w;
	if (x1 <= x0) return;
	for (int py = (y < by0 ? by0 : y); py < (y + h > by1 ? by1 : y + h); py++) {
		int yy = py - y;
		uint32_t       *drow = back->px + (long) py * back->stride;
		const uint32_t *srow = sc->px   + (long) (py - soy) * sc->stride - sox;
		const uint32_t *bdrow = backdrop ? backdrop->px + (long) py * backdrop->stride : drow;
		if (yy >= r && yy < h - r) {                  /* straight middle row: no AA */
			if (alpha >= 255)
				for (int px = x0; px < x1; px++) drow[px] = srow[px];
			else
				for (int px = x0; px < x1; px++) drow[px] = cmix(bdrow[px], srow[px], alpha);
			continue;
		}
		for (int px = x0; px < x1; px++) {            /* corner band: per-pixel AA coverage */
			int a = alpha, xx = px - x;
			int lx = xx < r ? r - 1 - xx : (xx >= w - r ? xx - (w - r) : -1);
			int ly = yy < r ? r - 1 - yy : (yy >= h - r ? yy - (h - r) : -1);
			if (lx >= 0 && ly >= 0) {
				float dx = lx + 0.5f, dy = ly + 0.5f;
				float e = (float) r - __builtin_sqrtf(dx * dx + dy * dy);
				int cov = e >= 0.5f ? 255 : (e <= -0.5f ? 0 : (int) ((e + 0.5f) * 255.0f));
				if (!cov) continue;
				a = cov * alpha / 255;
			}
			drow[px] = (a >= 255) ? srow[px] : cmix(bdrow[px], srow[px], a);
		}
	}
}

void nw_draw_cursor(const struct nw_surface *dst, int x, int y)
{
	for (int rr = 0; rr < NW_CURSOR_H; rr++) {
		const char *row = CURSOR[rr];
		for (int c = 0; row[c]; c++) {
			if (row[c] == 'X')      nw_put_pixel(dst, x + c, y + rr, COL_CURSOR_FG);
			else if (row[c] == '.') nw_put_pixel(dst, x + c, y + rr, COL_CURSOR_BG);
		}
	}
}

/* ---- top bar + dock -------------------------------------------------------------- */
/* The NanoOS "N" mark: two vertical bars + a diagonal, gradient-tinted, in a w*h box. */
static void draw_nanomark(const struct nw_surface *s, int x, int y, int sz)
{
	int bw = sz / 3;
	nw_vgrad_rect(s, x, y, bw, sz, 0x12a8f4, 0x7d3ff2);                 /* left  bar */
	nw_vgrad_rect(s, x + sz - bw, y, bw, sz, 0xff9d00, 0x6fd033);       /* right bar */
	for (int i = 0; i < sz; i++) {                                       /* diagonal */
		int dx = x + i * (sz - bw) / sz, dy = y + i;
		nw_blend_rect(s, dx, dy - bw / 2, bw, bw, 0x12a8f4, 255);
	}
}

/* macOS-style global menu bar: NanoOS logo (system menu) + the focused app's menu titles, and
 * a clock at the right. The open dropdown is drawn by draw_menu_dropdown (above everything). */
static void draw_panel(const struct nw_server *s, const struct nw_surface *back)
{
	int W = s->screen_w;
	nw_blend_rect(back, 0, 0, W, NW_PANEL_H, COL_PANEL, 205);          /* translucent tint */
	nw_blend_rect(back, 0, NW_PANEL_H - 1, W, 1, 0x9fb2cc, 140);        /* hairline */
	int y = (NW_PANEL_H - NW_FONT_H) / 2;
	if (s->menu_open && s->menu_which == NW_MENU_LOGO)               /* highlight the logo slot */
		nw_fill_round(back, 2, 2, 26, NW_PANEL_H - 4, 5, COL_TB_TOP, 90);
	draw_nanomark(back, 7, (NW_PANEL_H - 16) / 2, 16);

	const char *spec = (s->focus >= 0 && s->win[s->focus].used) ? s->win[s->focus].menu : "";
	int n = nw_menu_top_count(spec);
	if (n == 0) {                                                    /* no app menu: just a name */
		const char *app = (s->focus >= 0 && s->win[s->focus].used)
		                ? (s->win[s->focus].title[0] == '\x01' ? s->win[s->focus].title + 1
		                                                       : s->win[s->focus].title) : "NanoOS";
		nw_text(back, NW_MENU_X0, y, app && app[0] ? app : "NanoOS", COL_PANEL_FG);
	}
	for (int i = 0; i < n; i++) {
		int x, w; nw_menubar_top_x(s, i, &x, &w);
		int active = (s->menu_open && s->menu_which == i);
		if (active) nw_fill_round(back, x, 2, w, NW_PANEL_H - 4, 5, COL_TB_TOP, 110);
		char t[40]; nw_menu_top_title(spec, i, t, sizeof t);
		nw_text(back, x + 7, y, t, i == 0 ? COL_PANEL_FG : COL_PANEL_MUT);   /* app name bold-ish */
	}

	if (s->clock[0]) {                                               /* clock at the right */
		int cw = 0; while (s->clock[cw]) cw++;
		nw_text(back, W - cw * 8 - 12, y, s->clock, COL_PANEL_FG);
	}
}

/* The open menu dropdown, drawn last so it sits above the windows. */
static void draw_menu_dropdown(const struct nw_server *s, const struct nw_surface *back)
{
	if (!s->menu_open) return;
	int x, y, w, h; nw_menu_dropdown_rect(s, &x, &y, &w, &h);
	int n = nw_menu_open_item_count(s);
	nw_fill_round(back, x, y, w, h + 6, 9, 0x00f4f8fd, 246);
	nw_stroke_round(back, x, y, w, h + 6, 9, 0x00b8c6d8, 220);
	for (int i = 0; i < n; i++) {
		int iy = y + 4 + i * NW_MENU_ITEM_H;
		char lbl[64]; nw_menu_open_label(s, i, lbl, sizeof lbl);
		if (lbl[0] == '-' && lbl[1] == 0) { nw_blend_rect(back, x + 8, iy + NW_MENU_ITEM_H / 2, w - 16, 1, 0x00b8c6d8, 200); continue; }
		int hov = (i == s->menu_hover);
		if (hov) nw_fill_round(back, x + 3, iy, w - 6, NW_MENU_ITEM_H, 5, 0x0012a8f4, 255);
		nw_text(back, x + 12, iy + (NW_MENU_ITEM_H - NW_FONT_H) / 2, lbl, hov ? 0x00ffffff : 0x00172130);
	}
}

/* The taskbar: a full-width bar at the bottom with a Start button (NanoOS mark) and one button
 * per open window — Windows-style. The focused window's button is highlighted; a minimized
 * window's button is dimmed. Clicking is handled in nw_pointer (Start menu / minimize-restore). */
static void draw_taskbar(const struct nw_server *s, const struct nw_surface *back)
{
	int W = s->screen_w, y0 = s->screen_h - NW_TASK_H;
	nw_blend_rect(back, 0, y0, W, NW_TASK_H, COL_DOCK, 235);          /* the bar */
	nw_blend_rect(back, 0, y0, W, 1, 0x9fb2cc, 170);                  /* top hairline */

	/* Start button: the NanoOS "N" mark + "Start", highlighted while the Start menu is open. */
	int bx, by, bw, bh;
	nw_start_rect(s, &bx, &by, &bw, &bh);
	if (s->menu_open && s->menu_from_start) nw_blend_rect(back, bx, by, bw, bh, 0x12a8f4, 130);
	draw_nanomark(back, bx + 8, by + (bh - 16) / 2, 16);
	nw_text(back, bx + 30, by + (bh - NW_FONT_H) / 2, "Start", COL_PANEL_FG);

	/* one button per open window */
	int n = nw_task_count(s);
	for (int i = 0; i < n; i++) {
		int idx = nw_task_window(s, i);
		if (idx < 0) continue;
		const struct nw_window *w = &s->win[idx];
		nw_taskbar_button_rect(s, i, &bx, &by, &bw, &bh);
		int focused = (idx == s->focus) && !w->minimized;
		nw_fill_round(back, bx + 3, by + 5, bw - 6, bh - 10, 6,
		              focused ? 0x2b3650 : 0x1b2336, focused ? 255 : (w->minimized ? 120 : 205));
		const char *title = (w->title[0] == '\x01') ? w->title + 1 : w->title;
		nw_fill_round(back, bx + 10, by + (bh - 10) / 2, 10, 10, 3, focused ? 0x12a8f4 : 0x9fb2cc, 255);
		char t[19]; int k = 0; for (; title[k] && k < (int) sizeof t - 1; k++) t[k] = title[k]; t[k] = 0;
		nw_text(back, bx + 26, by + (bh - NW_FONT_H) / 2, t, focused ? 0xffffff : 0xc6d2e6);
	}
}

/* ---- the scene ------------------------------------------------------------------- */
/* Render every dirty window frame into its window-local cache, clearing the flag. Callers
 * (the shell, and tests) run this before nw_compose_scene so the cached frames are current;
 * a move (x/y change) leaves frames clean, so dragging recomposites from the cache with no
 * chrome/content re-render. Windows without a frame buffer are left to the live path below. */
void nw_render_dirty_frames(struct nw_server *s)
{
	for (int i = 0; i < NW_MAX_WINDOWS; i++) {
		struct nw_window *w = &s->win[i];
		if (!w->used || !w->frame || !w->frame_dirty)
			continue;
		struct nw_surface fs;
		fs.px = w->frame; fs.w = frame_w(w); fs.h = frame_h(w); fs.stride = frame_w(w);
		nw_surface_noclip(&fs);
		draw_window_to(&fs, w, (i == s->focus), 0, 0);   /* window-local: origin (0,0) */
		w->frame_dirty = 0;
	}
}

/* Fill bdc->bd over the window rect (x,y,w,h) with a blurred copy of `back` beneath it.
 * Samples a cache_rect (window expanded by the blur radius, clamped to the scissor+screen),
 * downsamples it into bdc->lo, blurs the small copy with nw_blur_rect, then bilinearly
 * upscales the window-rect portion back into bdc->bd. Returns 1 if bd was filled, else 0
 * (region empty, or lo scratch too small -> caller composites without a backdrop). */
/* allow_slide: during a drag, between forced rebuilds, reuse the last blur (resampled at the
 * moved rect's size) instead of recomputing — keeps the move fluid at the cost of an approximate
 * (slightly stretched) backdrop until the next forced rebuild snaps it back to exact. */
static int build_backdrop(const struct nw_surface *back, const struct nw_backdrop_ctx *bdc,
                          struct nw_window *w, int x, int y, int fw, int fh,
                          int force_rebuild, int allow_slide)
{
	int cx0, cy0, cx1, cy1;
	nw_surface_bounds(back, &cx0, &cy0, &cx1, &cy1);          /* honours the damage scissor */
	nw_rect clip = { cx0, cy0, cx1 - cx0, cy1 - cy0 };
	nw_rect want = nw_cache_rect((nw_rect){x, y, fw, fh}, bdc->radius, back->w, back->h);
	nw_rect cr = nw_rect_intersect(want, clip);
	if (nw_rect_empty(cr)) return 0;
	int f = bdc->factor, lw = want.w / f, lh = want.h / f;
	if (lw < 1 || lh < 1 || lw * lh > bdc->lo_cap) return 0;

	int reuse = w->bd_blur && !force_rebuild &&
	            nw_backdrop_reusable(w->bd_rect, want, w->bd_dirty) &&
	            w->bd_lw == lw && w->bd_lh == lh;
	if (!reuse) {
		if (allow_slide && w->bd_blur && w->bd_lw > 0) {
			lw = w->bd_lw; lh = w->bd_lh;       /* reuse last blur at its size (approx during drag) */
		} else {
			nw_downsample_box(back->px + (long) want.y * back->stride + want.x,
			                  lw * f, lh * f, back->stride, bdc->lo, f);
			struct nw_surface lo = { bdc->lo, lw, lh, lw, 0,0,0,0 };
			nw_surface_noclip(&lo);
			nw_blur_rect(&lo, 0, 0, lw, lh, bdc->radius / f > 0 ? bdc->radius / f : 1, bdc->passes);
			if (w->bd_blur) {                       /* persist into the per-window cache */
				for (int i = 0; i < lw * lh; i++) w->bd_blur[i] = bdc->lo[i];
				w->bd_lw = lw; w->bd_lh = lh; w->bd_rect = want; w->bd_dirty = 0;
			}
		}
	}
	const uint32_t *lo_src = reuse ? w->bd_blur : (w->bd_blur ? w->bd_blur : bdc->lo);
	nw_upsample_bilinear(lo_src, lw, lh,
	                     bdc->bd->px + (long) want.y * bdc->bd->stride + want.x,
	                     want.w - (want.w % f), want.h - (want.h % f), bdc->bd->stride);
	return 1;
}

void nw_compose_scene(const struct nw_server *s, const struct nw_surface *back,
                      const struct nw_surface *scratch, const struct nw_surface *wall,
                      const struct nw_backdrop_ctx *bdc)
{
	if (wall) nw_blit(back, 0, 0, wall, 0, 0, back->w, back->h);
	else      nw_fill_rect(back, 0, 0, back->w, back->h, 0x1e2a3a);

	for (int z = 0; z < s->zn; z++) {
		int idx = s->zorder[z];
		const struct nw_window *w = &s->win[idx];
		if (!w->used || w->minimized) continue;       /* minimized windows live only on the taskbar */
		int fw = frame_w(w), fh = frame_h(w), focused = (idx == s->focus);
		int alpha = (w->title[0] == '\x01') ? DARK_ALPHA : WIN_ALPHA;
		int force = 0;
		if (bdc && idx == bdc->drag_win)
			force = (bdc->frame_ctr % NW_BD_FASTDRAG_N) == 0;
		int allow_slide = bdc && idx == bdc->drag_win && !force;
		const struct nw_surface *bd = 0;
		if (bdc && bdc->bd && w->glass &&
		    build_backdrop(back, bdc, (struct nw_window *) w, w->x, w->y, fw, fh, force, allow_slide))
			bd = bdc->bd;
		if (w->frame) {                          /* cached frame: composite window-local source */
			struct nw_surface fs;
			fs.px = w->frame; fs.w = fw; fs.h = fh; fs.stride = fw;
			nw_surface_noclip(&fs);
			composite_round(back, &fs, w->x, w->y, fw, fh, NW_RADIUS, alpha, w->x, w->y, bd);
			nw_stroke_round(back, w->x, w->y, fw, fh, NW_RADIUS, COL_BORDER, 150);
		} else if (scratch) {                    /* screen-space scratch: render live + composite */
			draw_window_to(scratch, w, focused, w->x, w->y);
			composite_round(back, scratch, w->x, w->y, fw, fh, NW_RADIUS, alpha, 0, 0, bd);
			nw_stroke_round(back, w->x, w->y, fw, fh, NW_RADIUS, COL_BORDER, 150);
		} else {
			draw_window_to(back, w, focused, w->x, w->y);     /* simple/host path: opaque, square */
		}
	}
	draw_panel(s, back);
	draw_taskbar(s, back);
	draw_menu_dropdown(s, back);                  /* the open menu, above the windows */

	if (s->run_open) {                            /* Super+R launcher, above everything */
		int x, y, w, h;
		nw_run_rect(s, &x, &y, &w, &h);
		nw_fill_round(back, x, y, w, h, 12, 0x1b2433, 240);
		nw_stroke_round(back, x, y, w, h, 12, 0xffffff, 60);
		int tx = x + 14, ty = y + (h - NW_FONT_H) / 2;
		tx = nw_text(back, tx, ty, "Run:  ", 0x9fb0c0);
		char buf[NW_RUN_MAX];
		int nlen = s->run_len; if (nlen > NW_RUN_MAX - 1) nlen = NW_RUN_MAX - 1;
		for (int i = 0; i < nlen; i++) buf[i] = s->run_text[i];
		buf[nlen] = 0;
		tx = nw_text(back, tx, ty, buf, 0xffffff);
		nw_blend_rect(back, tx, ty, 2, NW_FONT_H, 0xc0c0c0, 255);
	}
}

void nw_compose(const struct nw_server *s, const struct nw_surface *back)
{
	nw_compose_scene(s, back, 0, 0, 0);
	nw_draw_cursor(back, s->cursor_x, s->cursor_y);
}
