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
#define WIN_ALPHA      255        /* opaque body -> interior composite is a fast copy */
#define DARK_ALPHA     255

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

/* Render the window opaquely (material + title bar + controls + content) into `scratch`,
 * at its on-screen position. The rounded/alpha composite happens afterwards. */
static void draw_window_to(const struct nw_surface *sc, const struct nw_window *w, int focused)
{
	int fw = frame_w(w), fh = frame_h(w);
	int dark = (w->title[0] == '\x01');       /* a leading 0x01 in the title flags a dark window */
	uint32_t mat = dark ? COL_WIN_DARK : COL_WIN_LIGHT;
	const char *title = dark ? w->title + 1 : w->title;

	nw_fill_rect(sc, w->x, w->y, fw, fh, mat);                       /* material */
	nw_vgrad_rect(sc, w->x, w->y, fw, NW_TITLEBAR_H,                 /* title bar */
	              dark ? COL_TB_DTOP : COL_TB_TOP, dark ? COL_TB_DBOT : COL_TB_BOT);
	if (!focused)                                                    /* dim the bar when unfocused */
		nw_blend_rect(sc, w->x, w->y, fw, NW_TITLEBAR_H, mat, 80);

	/* title text (with a little app dot to the left) */
	uint32_t tfg = dark ? COL_TITLE_DFG : COL_TITLE_FG;
	int ty = w->y + (NW_TITLEBAR_H - NW_FONT_H) / 2;
	nw_fill_round(sc, w->x + 10, ty + 2, 12, 12, 3, focused ? 0x12a8f4 : 0x9fb2cc, 255);
	nw_text(sc, w->x + 28, ty, title, tfg);                 /* bg 0 = ignored (opaque text bg) */

	/* window controls on the right: —  □  × */
	uint32_t cfg = dark ? COL_CTRL_D : COL_CTRL;
	int cw = NW_CLOSE, ch2 = NW_TITLEBAR_H;
	int x3 = w->x + fw - cw;                       /* × */
	int x2 = x3 - cw;                              /* □ */
	int x1 = x2 - cw;                              /* — */
	int gy = w->y + ch2 / 2;
	nw_blend_rect(sc, x1 + cw / 2 - 4, gy, 8, 2, cfg, 255);                       /* minimize */
	nw_stroke_round(sc, x2 + cw / 2 - 5, gy - 5, 10, 10, 2, cfg, 255);            /* maximize */
	for (int i = -4; i <= 4; i++) {                                               /* close × */
		nw_blend_pixel(sc, x3 + cw / 2 + i, gy - 4 + (i + 4), cfg, 255);
		nw_blend_pixel(sc, x3 + cw / 2 + i, gy + 4 - (i + 4), cfg, 255);
	}

	/* content */
	int cox = w->x + NW_BORDER, coy = w->y + NW_TITLEBAR_H;
	if (w->buf) {
		struct nw_surface src;
		src.px = w->buf; src.w = w->cw; src.h = w->ch; src.stride = w->cw;
		nw_surface_noclip(&src);
		nw_blit(sc, cox, coy, &src, 0, 0, w->cw, w->ch);
	} else {
		nw_fill_rect(sc, cox, coy, w->cw, w->ch, mat);
	}
}

/* Composite scratch[winrect] onto `back` with rounded corners (AA) + per-window alpha. Fast:
 * the clip bounds are resolved once, interior rows are a straight copy/blend, and only the two
 * corner bands (the top r and bottom r rows) pay the per-pixel anti-aliased coverage. */
static void composite_round(const struct nw_surface *back, const struct nw_surface *sc,
                            int x, int y, int w, int h, int r, int alpha)
{
	int bx0, by0, bx1, by1;
	nw_surface_bounds(back, &bx0, &by0, &bx1, &by1);
	int x0 = x < bx0 ? bx0 : x, x1 = x + w > bx1 ? bx1 : x + w;
	for (int py = (y < by0 ? by0 : y); py < (y + h > by1 ? by1 : y + h); py++) {
		int yy = py - y;
		int corner_row = (yy < r || yy >= h - r);
		uint32_t       *drow = back->px + (long) py * back->stride;
		const uint32_t *srow = sc->px   + (long) py * sc->stride;
		for (int px = x0; px < x1; px++) {
			int a = alpha;
			if (corner_row) {                         /* anti-aliased rounded corner */
				int xx = px - x;
				int lx = xx < r ? r - 1 - xx : (xx >= w - r ? xx - (w - r) : -1);
				int ly = yy < r ? r - 1 - yy : (yy >= h - r ? yy - (h - r) : -1);
				if (lx >= 0 && ly >= 0) {
					float dx = lx + 0.5f, dy = ly + 0.5f;
					float e = (float) r - __builtin_sqrtf(dx * dx + dy * dy);
					int cov = e >= 0.5f ? 255 : (e <= -0.5f ? 0 : (int) ((e + 0.5f) * 255.0f));
					if (!cov) continue;
					a = cov * alpha / 255;
				}
			}
			drow[px] = (a >= 255) ? srow[px] : nw_mix(drow[px], srow[px], a);
		}
	}
}

/* A soft drop shadow as concentric 1px rounded-rect RINGS (perimeter cost, not area cost), so
 * it stays cheap and — crucially — never fills the window interior, so a translucent window
 * isn't darkened by the shadow underneath it. Offset down a few px for a "drop". */
static void draw_shadow(const struct nw_surface *back, int x, int y, int w, int h, int r)
{
	for (int i = 1; i <= 8; i++) {
		int a = 28 - i * 3;
		if (a <= 0) break;
		nw_stroke_round(back, x - i, y - i + 5, w + 2 * i, h + 2 * i, r + i, 0x0a1830, a);
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

static void draw_panel(const struct nw_server *s, const struct nw_surface *back, const char *app)
{
	int W = s->screen_w;
	if (s->drag_win < 0)                                               /* skip the blur mid-drag */
		nw_blur_rect(back, 0, 0, W, NW_PANEL_H, 6, 1);                 /* frosted backdrop */
	nw_blend_rect(back, 0, 0, W, NW_PANEL_H, COL_PANEL, 205);
	nw_blend_rect(back, 0, NW_PANEL_H - 1, W, 1, 0x9fb2cc, 140);        /* hairline */
	int y = (NW_PANEL_H - NW_FONT_H) / 2;
	draw_nanomark(back, 9, (NW_PANEL_H - 16) / 2, 16);
	int x = 34;
	x = nw_text(back, x, y, app && app[0] ? app : "NanoOS", COL_PANEL_FG) + 14;
	static const char *const M[] = { "File", "Edit", "View", "Window", "Help" };
	for (int i = 0; i < 5; i++) x = nw_text(back, x, y, M[i], COL_PANEL_MUT) + 14;

	/* right side: Quit / Shutdown pills + a clock placeholder */
	int rx, ry, rw, rh;
	nw_panel_button_rect(s, NW_PANEL_SHUTDOWN, &rx, &ry, &rw, &rh);
	nw_fill_round(back, rx, ry, rw, rh, 6, 0xff6b6b, 235);
	nw_text(back, rx + 8, ry + (rh - NW_FONT_H) / 2, "Shutdown", 0xffffff);
	nw_panel_button_rect(s, NW_PANEL_QUIT, &rx, &ry, &rw, &rh);
	nw_fill_round(back, rx, ry, rw, rh, 6, 0xffffff, 150);
	nw_text(back, rx + 8, ry + (rh - NW_FONT_H) / 2, "Quit", COL_PANEL_FG);
}

static void draw_dock(const struct nw_server *s, const struct nw_surface *back)
{
	const uint32_t ic[6] = { 0x1797ff, 0x161b2a, 0xff8500, 0x7139e8, 0x43b548, 0xbcc6d5 };
	int n = 6, slot = 52, pad = 12;
	int dw = n * slot + 2 * pad, dh = NW_DOCK_H;
	int dx = (s->screen_w - dw) / 2, dy = s->screen_h - dh - 10;
	draw_shadow(back, dx, dy, dw, dh, 20);
	if (s->drag_win < 0)
		nw_blur_rect(back, dx, dy, dw, dh, 7, 1);                      /* frosted dock backdrop */
	nw_fill_round(back, dx, dy, dw, dh, 20, COL_DOCK, 150);
	nw_stroke_round(back, dx, dy, dw, dh, 20, 0xffffff, 180);
	for (int i = 0; i < n; i++) {
		int ix = dx + pad + i * slot + 4, iy = dy + (dh - 44) / 2;
		nw_fill_round(back, ix, iy, 44, 44, 12, ic[i], 245);
		nw_stroke_round(back, ix, iy, 44, 44, 12, 0xffffff, 60);
	}
}

/* ---- the scene ------------------------------------------------------------------- */
void nw_compose_scene(const struct nw_server *s, const struct nw_surface *back,
                      const struct nw_surface *scratch, const struct nw_surface *wall)
{
	if (wall) nw_blit(back, 0, 0, wall, 0, 0, back->w, back->h);
	else      nw_fill_rect(back, 0, 0, back->w, back->h, 0x1e2a3a);

	const char *app = "NanoOS";
	for (int z = 0; z < s->zn; z++) {
		int idx = s->zorder[z];
		const struct nw_window *w = &s->win[idx];
		if (!w->used) continue;
		int fw = frame_w(w), fh = frame_h(w), focused = (idx == s->focus);
		if (scratch) {
			draw_shadow(back, w->x, w->y, fw, fh, NW_RADIUS);
			draw_window_to(scratch, w, focused);
			composite_round(back, scratch, w->x, w->y, fw, fh, NW_RADIUS,
			                (w->title[0] == '\x01') ? DARK_ALPHA : WIN_ALPHA);
			nw_stroke_round(back, w->x, w->y, fw, fh, NW_RADIUS, COL_BORDER, 150);
		} else {
			draw_window_to(back, w, focused);     /* simple/host path: opaque, square */
		}
		if (focused) app = (w->title[0] == '\x01') ? w->title + 1 : w->title;
	}
	draw_panel(s, back, app);
	draw_dock(s, back);

	if (s->run_open) {                            /* Super+R launcher, above everything */
		int x, y, w, h;
		nw_run_rect(s, &x, &y, &w, &h);
		draw_shadow(back, x, y, w, h, 12);
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
	nw_compose_scene(s, back, 0, 0);
	nw_draw_cursor(back, s->cursor_x, s->cursor_y);
}
