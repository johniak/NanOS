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
#include "nwfont.h"
#include <string.h>

/* VGA 1-bit fallback glyphs, shared with nw_gfx.c/nterm/vtfont (definition lives in vtfont.c,
 * already linked into every binary that pulls in nw_gfx.o for nw_text/nw_draw_text). Only used
 * below when no TTF is loaded, matching nw_text's own fallback. */
extern const unsigned char nx_font8x16[256][16];

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
#define WIN_ALPHA      206        /* translucent "glass" body over the pre-blurred backdrop */
#define DARK_ALPHA     214

#define COL_PANEL      0xeef7ff   /* top bar tint (translucent)       */
#define COL_PANEL_FG   0x141d2e
#define COL_PANEL_MUT  0x5d6b80
#define COL_DOCK       0xeef8ff
#define COL_CURSOR_FG  0x101620
#define COL_CURSOR_BG  0xffffff

/* Glass-mode chrome ink (Task 9): the menubar/taskbar backgrounds go transparent-key black — the GL
 * compositor paints the actual dark-glass band underneath (nw_compose_gl.c FS_BAR) — so these must
 * be unambiguously bright: FS_KEYED discards a pixel only when ALL of r,g,b are within ~0.02 (of 1.0,
 * ~5/255) of pure black, otherwise it is drawn fully opaque (no partial alpha under the key). */
#define NW_GLASS_INK_FG   0xf0f5fc   /* crisp white: primary labels, clock, Start text */
#define NW_GLASS_INK_MUT  0xaab4c4   /* muted-but-visible: inactive menu titles, pill rings */
#define NW_GLASS_INK_FILL 0x3a4456   /* focused taskbar-button fill (brighter than the classic pill) */

/* Runtime theme (set by the shell from settings.yaml; defaults match the compiled-in look).
 * Module-static because the per-window frame cache render (draw_window_to) and the live scene
 * compose both consult them without a server handle in scope. */
static uint32_t s_accent = 0x12a8f4u;   /* UI accent: focus dots, highlights, the N mark */
static int      s_radius = NW_RADIUS;    /* window corner radius (applied at composite time) */
static int      s_shadow = 1;            /* draw window drop shadows */

void nw_compose_set_theme(uint32_t accent, int radius, int shadow)
{
	s_accent = accent ? accent : 0x12a8f4u;
	s_radius = (radius >= 0 && radius <= 20) ? radius : NW_RADIUS;
	s_shadow = shadow ? 1 : 0;
}

/* GL liquid-glass windows: when set (GL compositor live), the frame band (titlebar + borders) is
 * rendered as pure key black with only the ink (centred glowing title) on top; the GL shader puts
 * the glass slab under that ink and draws the caption spheres itself. OFF = classic opaque CPU
 * frame (fallback path unchanged). */
static int s_glass_frame = 0;
void nw_compose_set_glass_frame(int on) { s_glass_frame = on ? 1 : 0; }

/* Classic 11x16 arrow cursor: 'X' outline, '.' fill, ' ' transparent. */
static const char *const CURSOR[16] = {
	"X          ", "XX         ", "X.X        ", "X..X       ",
	"X...X      ", "X....X     ", "X.....X    ", "X......X   ",
	"X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ",
	"X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     XXX   "
};

/* ---- wallpaper (rendered once) --------------------------------------------------- */
/* Colourful radial blobs over a soft diagonal gradient — the Nano OS desktop. */
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

/* Live reference to the currently-shown wallpaper surface, for auto ink polarity sampling below.
 * Set once by the shell (nwm.c, right after it renders/caches the wallpaper) — module-static for
 * the same reason as s_accent/s_radius/s_glass_frame: draw_window_to has no server handle. */
static const struct nw_surface *s_wall;

void nw_compose_set_wallpaper_ref(const struct nw_surface *wall) { s_wall = wall; }

/* Sample a sparse 8x2 grid of wallpaper pixels under rect (x,y,w,h); gamma-space luma
 * Y=(77R+150G+29B)>>8, threshold 117 (~WCAG 0.179 linear) with +/-8 hysteresis so a window
 * dragged across a light/dark wallpaper boundary doesn't flicker ink polarity every frame. */
int nw_backdrop_wants_dark_ink(int x, int y, int w, int h, int prev)
{
	if (!s_wall || w < 8 || h < 2) return prev >= 0 ? prev : 1;
	long acc = 0; int n = 0;
	for (int j = 0; j < 2; j++) for (int i = 0; i < 8; i++) {
		int sx = x + (w * (2 * i + 1)) / 16, sy = y + (h * (2 * j + 1)) / 4;
		if (sx < 0 || sy < 0 || sx >= s_wall->w || sy >= s_wall->h) continue;
		uint32_t p = s_wall->px[(size_t) sy * s_wall->stride + sx];
		acc += (77 * ((p >> 16) & 0xff) + 150 * ((p >> 8) & 0xff) + 29 * (p & 0xff)) >> 8;
		n++;
	}
	if (!n) return prev >= 0 ? prev : 1;
	/* The title sits on the light glass SLAB, not on the raw wallpaper: the slab's frost,
	 * tint and sheen lift the backdrop by roughly a 45% coat of near-white before ink
	 * lands on it (mockup: light windows carry dark ink + white glow even over a navy
	 * wallpaper). Composite that lift into the sampled luma, then threshold — only a
	 * genuinely near-black wallpaper flips a light window to light ink. */
	int luma = (int) (acc / n);
	luma = (luma * 115 + 235 * 141) >> 8;       /* luma*0.45 + 235*0.55 (sheen is strongest up top) */
	if (prev == 1 && luma < 109) return 0;      /* hysteresis band 109..125 */
	if (prev == 0 && luma > 125) return 1;
	if (prev < 0) return luma > 117;
	return prev;
}

/* ---- Aero caption glow (glass frame only) ----------------------------------------- */
/* GLOW_R: box-blur radius (px) for the first, softest pass (halved for the second pass).
 * GLOW_MAXW: hard cap on the glow buffer width — long titles get clipped to this minus the
 * blur padding, never overflow the static buffers below (no per-frame allocation). */
#define GLOW_R    6
#define GLOW_MAXW 512

/* Rasterize `str`'s glyph coverage into `cov[][GLOW_MAXW]` (row-major, physical width GLOW_MAXW,
 * physical height `h` <= NW_TITLEBAR_H), MAX-combined so overlapping coverage never wraps or
 * double-counts. Same glyph walk/metrics as nw_text/nw_text_argb (byte-at-a-time, no UTF-8
 * decode, nwfont_get(NWFONT_UI, cp) + advance, baseline = ty0 + ascent) but writes into a plain
 * byte buffer instead of blending onto a surface. `x0`/`ty0` are LOCAL to the buffer (x0 = left
 * pen start, ty0 = line-box top) — the caller (draw_caption_glow) adds the frame's screen/window
 * offset only once, at the final composite. Falls back to the 1-bit VGA font (full 255 coverage
 * per set pixel) when no TTF is loaded, matching nw_text's own fallback. */
static void rasterize_run_coverage(uint8_t cov[][GLOW_MAXW], int h, int x0, int ty0, const char *str)
{
	int x = x0;
	if (nwfont_loaded(NWFONT_UI)) {
		int baseline = ty0 + nwfont_ascent(NWFONT_UI);
		for (; *str; str++) {
			const struct nwfont_glyph *g = nwfont_get(NWFONT_UI, (unsigned char) *str);
			if (!g) continue;
			if (g->cov) {
				for (int gy = 0; gy < g->h; gy++) {
					int py = baseline + g->top + gy;
					if (py < 0 || py >= h) continue;
					const unsigned char *covrow = g->cov + (long) gy * g->w;
					for (int gx = 0; gx < g->w; gx++) {
						int px = x + g->bx + gx;
						if (px < 0 || px >= GLOW_MAXW) continue;
						unsigned char c = covrow[gx];
						if (c > cov[py][px]) cov[py][px] = c;
					}
				}
			}
			x += g->advance;
		}
		return;
	}
	for (; *str; str++) {
		const unsigned char *glyph = nx_font8x16[(unsigned char) *str];
		for (int row = 0; row < NW_FONT_H; row++) {
			int py = ty0 + row;
			if (py < 0 || py >= h) continue;
			unsigned char bits = glyph[row];
			for (int col = 0; col < NW_FONT_W; col++) {
				int px = x + col;
				if (px < 0 || px >= GLOW_MAXW) continue;
				if ((bits & (0x80u >> col)) && cov[py][px] != 255) cov[py][px] = 255;
			}
		}
		x += NW_FONT_W;
	}
}

/* Running-sum box blur, radius r, horizontal/vertical. Clamped window at the ends (the divisor
 * shrinks near an edge instead of treating out-of-range samples as 0), so the glow doesn't dim at
 * the padded buffer edges. `src`/`dst` may alias (safe even when called box_blur_*(buf, buf, ...)
 * for a second pass in place): each row/column is copied into a small local scratch first, so the
 * running sum never reads a value this same pass already overwrote. O(w*h) total, no allocation. */
static void box_blur_h(uint8_t src[][GLOW_MAXW], uint8_t dst[][GLOW_MAXW], int w, int h, int r)
{
	uint8_t row[GLOW_MAXW];
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) row[x] = src[y][x];
		int sum = 0, cnt = 0;
		for (int k = 0; k <= r && k < w; k++) { sum += row[k]; cnt++; }
		for (int x = 0; x < w; x++) {
			dst[y][x] = (uint8_t) (sum / cnt);
			int add = x + r + 1, rem = x - r;
			if (add < w)  { sum += row[add]; cnt++; }
			if (rem >= 0) { sum -= row[rem]; cnt--; }
		}
	}
}

static void box_blur_v(uint8_t src[][GLOW_MAXW], uint8_t dst[][GLOW_MAXW], int w, int h, int r)
{
	uint8_t col[NW_TITLEBAR_H];
	for (int x = 0; x < w; x++) {
		for (int y = 0; y < h; y++) col[y] = src[y][x];
		int sum = 0, cnt = 0;
		for (int k = 0; k <= r && k < h; k++) { sum += col[k]; cnt++; }
		for (int y = 0; y < h; y++) {
			dst[y][x] = (uint8_t) (sum / cnt);
			int add = y + r + 1, rem = y - r;
			if (add < h)  { sum += col[add]; cnt++; }
			if (rem >= 0) { sum -= col[rem]; cnt--; }
		}
	}
}

/* Aero caption glow: rasterize the title run's coverage once, box-blur it twice (r=6 then r=3,
 * ~ Gaussian), gain it into a soft round sheet, then composite sheet-then-core with real alpha
 * (nw_over_pixel — straight ARGB, so the GL shader's ctex.a carries the glow's own coverage; see
 * Step 5 in nw_compose_gl.c). dark_ink=1: light sheet + dark core (legible over light glass/
 * wallpaper); dark_ink=0: dark sheet + light core (dark glass, e.g. Terminal, or a dark wallpaper
 * area). Unfocused windows get a lower gain (2x not 3x) and a dimmer core (x200/256).
 *
 * `cx` is the frame's horizontal centre and `oy` its top, both in `sc`'s OWN coordinate space —
 * (0, 0)-relative for the window-local frame-cache render, or (w->x, w->y)-relative for the
 * screen-space scratch render (matching how the classic ±1px halo above folds ox/oy into `ty`
 * before drawing). The buffer itself is always rendered LOCAL (baseline row is a fixed offset
 * within the title bar, independent of oy); `oy` is added back exactly once, at the final
 * nw_over_pixel calls, so the glow lands in the right place in either coordinate space. */
static void draw_caption_glow(const struct nw_surface *sc, int cx, int oy,
                              const char *title, int dark_ink, int focused)
{
	static uint8_t cov[NW_TITLEBAR_H][GLOW_MAXW], tmp[NW_TITLEBAR_H][GLOW_MAXW];
	int tw = nw_text_w(title);
	if (tw > GLOW_MAXW - 4 * GLOW_R) tw = GLOW_MAXW - 4 * GLOW_R;
	int W = tw + 4 * GLOW_R, H = NW_TITLEBAR_H;
	int ty0 = (NW_TITLEBAR_H - NW_FONT_H) / 2;      /* local line-box top (== classic `ty - oy`) */
	memset(cov, 0, sizeof(cov));
	rasterize_run_coverage(cov, H, 2 * GLOW_R, ty0, title);
	box_blur_h(cov, tmp, W, H, GLOW_R);    box_blur_v(tmp, tmp, W, H, GLOW_R);
	box_blur_h(tmp, tmp, W, H, GLOW_R / 2); box_blur_v(tmp, tmp, W, H, GLOW_R / 2);

	uint32_t sheet = dark_ink ? 0x00f2f6fa : 0x0010151f;
	uint32_t core  = dark_ink ? 0x001a2330 : 0x00f0f4f8;
	int gain = focused ? 4 : 2;   /* was 3: too subtle over light backdrops (Task 8 gap) */
	int x0 = cx - W / 2;
	for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++) {
		int a = tmp[yy][xx] * gain; if (a > 255) a = 255;
		if (a) nw_over_pixel(sc, x0 + xx, oy + yy, ((uint32_t) a << 24) | sheet);
	}
	const uint8_t *lut = nw_cov143();
	for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++) {
		int a = lut[cov[yy][xx]]; if (!focused) a = (a * 200) >> 8;
		if (a) nw_over_pixel(sc, x0 + xx, oy + yy, ((uint32_t) a << 24) | core);
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

	if (s_glass_frame) {
		/* real-alpha ink canvas: transparent band, the glow + core below carry their own alpha
		 * (Step 5 in nw_compose_gl.c reads ctex.a directly — no luminance keying anymore). */
		nw_clear_argb(sc, ox, oy, fw, fh, 0x00000000u);
	} else {
		nw_fill_rect(sc, ox, oy, fw, fh, mat);                 /* material */
		nw_vgrad_rect(sc, ox, oy, fw, NW_TITLEBAR_H,           /* title bar */
		              dark ? COL_TB_DTOP : COL_TB_TOP, dark ? COL_TB_DBOT : COL_TB_BOT);
		if (!focused)                                          /* dim the bar when unfocused */
			nw_blend_rect(sc, ox, oy, fw, NW_TITLEBAR_H, mat, 80);
	}

	/* title text (with a little app dot to the left) */
	uint32_t tfg = dark ? COL_TITLE_DFG : COL_TITLE_FG;
	int ty = oy + (NW_TITLEBAR_H - NW_FONT_H) / 2;
	if (s_glass_frame) {
		/* real Aero glow: blurred coverage sheet + crisp core, polarity from w->ink_dark
		 * (refreshed in nw_render_dirty_frames from what's under the bar) — dark windows
		 * (Terminal-style) always get a light core regardless of the sampled backdrop. */
		int dark_ink = (w->ink_dark != 0) && !dark;
		draw_caption_glow(sc, ox + fw / 2, oy, title, dark_ink, focused);
	} else {
		nw_fill_round(sc, ox + 10, ty + 2, 12, 12, 3, focused ? s_accent : 0x9fb2cc, 255);
		nw_text(sc, ox + 28, ty, title, tfg);
	}

	if (!s_glass_frame) {
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
/* The Nano OS "N" mark: two vertical bars + a diagonal, gradient-tinted, in a w*h box. */
static void draw_nanomark(const struct nw_surface *s, int x, int y, int sz)
{
	int bw = sz / 3;
	nw_vgrad_rect(s, x, y, bw, sz, s_accent, 0x7d3ff2);                 /* left  bar */
	nw_vgrad_rect(s, x + sz - bw, y, bw, sz, 0xff9d00, 0x6fd033);       /* right bar */
	for (int i = 0; i < sz; i++) {                                       /* diagonal */
		int dx = x + i * (sz - bw) / sz, dy = y + i;
		nw_blend_rect(s, dx, dy - bw / 2, bw, bw, s_accent, 255);
	}
}

/* macOS-style global menu bar: Nano OS logo (system menu) + the focused app's menu titles, and
 * a clock at the right. The open dropdown is drawn by draw_menu_dropdown (above everything). */
static void draw_panel(const struct nw_server *s, const struct nw_surface *back)
{
	int W = s->screen_w;
	/* Glass mode: the GL compositor already painted a dark-glass band + hairline UNDER this overlay
	 * (nw_compose_gl.c's FS_BAR, drawn after the scene, before this ink is keyed on) — so leave the
	 * background at the overlay's transparent key black and skip the CPU hairline (the GL one is the
	 * physically-lit edge). Ink switches to bright colours that clear the chrome key (see
	 * nw_compose_chrome / FS_KEYED: any RGB not within ~0.02 of pure black survives, fully opaque —
	 * there is no partial alpha under the key, so ink must be unambiguously bright, not just tinted). */
	if (!s_glass_frame) {
		nw_blend_rect(back, 0, 0, W, NW_PANEL_H, COL_PANEL, 205);          /* translucent tint */
		nw_blend_rect(back, 0, NW_PANEL_H - 1, W, 1, 0x9fb2cc, 140);        /* hairline */
	}
	uint32_t fg = s_glass_frame ? NW_GLASS_INK_FG : COL_PANEL_FG;
	uint32_t mut = s_glass_frame ? NW_GLASS_INK_MUT : COL_PANEL_MUT;
	int y = (NW_PANEL_H - NW_FONT_H) / 2;
	if (s->menu_open && s->menu_which == NW_MENU_LOGO)               /* highlight the logo slot */
		nw_fill_round(back, 2, 2, 26, NW_PANEL_H - 4, 5, COL_TB_TOP, 90);
	draw_nanomark(back, 7, (NW_PANEL_H - 16) / 2, 16);

	const char *spec = (s->focus >= 0 && s->win[s->focus].used) ? s->win[s->focus].menu : "";
	int n = nw_menu_top_count(spec);
	if (n == 0) {                                                    /* no app menu: just a name */
		const char *app = (s->focus >= 0 && s->win[s->focus].used)
		                ? (s->win[s->focus].title[0] == '\x01' ? s->win[s->focus].title + 1
		                                                       : s->win[s->focus].title) : "Nano OS";
		nw_text(back, NW_MENU_X0, y, app && app[0] ? app : "Nano OS", fg);
	}
	for (int i = 0; i < n; i++) {
		int x, w; nw_menubar_top_x(s, i, &x, &w);
		int active = (s->menu_open && s->menu_which == i);
		if (active) nw_fill_round(back, x, 2, w, NW_PANEL_H - 4, 5, COL_TB_TOP, 110);
		char t[40]; nw_menu_top_title(spec, i, t, sizeof t);
		nw_text(back, x + 7, y, t, i == 0 ? fg : mut);   /* app name bold-ish */
	}

	if (s->clock[0]) {                                               /* clock at the right */
		int cw = 0; while (s->clock[cw]) cw++;
		nw_text(back, W - cw * 8 - 12, y, s->clock, fg);
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

/* The taskbar: a full-width bar at the bottom with a Start button (Nano OS mark) and one button
 * per open window — Windows-style. The focused window's button is highlighted; a minimized
 * window's button is dimmed. Clicking is handled in nw_pointer (Start menu / minimize-restore). */
static void draw_taskbar(const struct nw_server *s, const struct nw_surface *back)
{
	int W = s->screen_w, y0 = s->screen_h - NW_TASK_H;
	/* Glass mode: background stays transparent-key black (the GL dark-glass band shows through) and
	 * the top hairline is skipped (the GL band already draws its own, at the physically correct
	 * lit edge) — see draw_panel's comment for the same rationale. */
	if (!s_glass_frame) {
		nw_blend_rect(back, 0, y0, W, NW_TASK_H, COL_DOCK, 235);          /* the bar */
		nw_blend_rect(back, 0, y0, W, 1, 0x9fb2cc, 170);                  /* top hairline */
	}
	uint32_t fg = s_glass_frame ? NW_GLASS_INK_FG : COL_PANEL_FG;

	/* Start button: the Nano OS "N" mark + "Start", highlighted while the Start menu is open. */
	int bx, by, bw, bh;
	nw_start_rect(s, &bx, &by, &bw, &bh);
	if (s->menu_open && s->menu_from_start) nw_blend_rect(back, bx, by, bw, bh, s_accent, 130);
	draw_nanomark(back, bx + 8, by + (bh - 16) / 2, 16);
	nw_text(back, bx + 30, by + (bh - NW_FONT_H) / 2, "Start", fg);

	/* one button per open window */
	int n = nw_task_count(s);
	for (int i = 0; i < n; i++) {
		int idx = nw_task_window(s, i);
		if (idx < 0) continue;
		const struct nw_window *w = &s->win[idx];
		nw_taskbar_button_rect(s, i, &bx, &by, &bw, &bh);
		int focused = (idx == s->focus) && !w->minimized;
		if (s_glass_frame) {
			/* pills read as bright hairline rings over the dark glass band; the focused button also
			 * gets a brighter fill so it stays "visibly highlighted" the way the classic pill was. */
			if (focused) nw_fill_round(back, bx + 3, by + 5, bw - 6, bh - 10, 6, NW_GLASS_INK_FILL, 255);
			nw_stroke_round(back, bx + 3, by + 5, bw - 6, bh - 10, 6, NW_GLASS_INK_MUT,
			                w->minimized ? 140 : 255);
		} else {
			nw_fill_round(back, bx + 3, by + 5, bw - 6, bh - 10, 6,
			              focused ? 0x2b3650 : 0x1b2336, focused ? 255 : (w->minimized ? 120 : 205));
		}
		const char *title = (w->title[0] == '\x01') ? w->title + 1 : w->title;
		nw_fill_round(back, bx + 10, by + (bh - 10) / 2, 10, 10, 3, focused ? s_accent : 0x9fb2cc, 255);
		char t[19]; int k = 0; for (; title[k] && k < (int) sizeof t - 1; k++) t[k] = title[k]; t[k] = 0;
		nw_text(back, bx + 26, by + (bh - NW_FONT_H) / 2, t,
		        focused ? (s_glass_frame ? NW_GLASS_INK_FG : 0xffffff) : (s_glass_frame ? NW_GLASS_INK_MUT : 0xc6d2e6));
	}
}

/* ---- the scene ------------------------------------------------------------------- */
/* Render every dirty window frame into its window-local cache, clearing the flag. Callers
 * (the shell, and tests) run this before nw_compose_scene so the cached frames are current;
 * a move (x/y change) leaves frames clean, so dragging recomposites from the cache with no
 * chrome/content re-render. Windows without a frame buffer are left to the live path below. */
/* Monotonic render generation. Every actual frame re-render stamps the window with a globally
 * unique value, so the GL compositor's per-window "last uploaded gen" can never coincidentally
 * match a different window that happens to reuse the same slot index. */
static unsigned g_render_gen;

void nw_render_dirty_frames(struct nw_server *s)
{
	for (int i = 0; i < NW_MAX_WINDOWS; i++) {
		struct nw_window *w = &s->win[i];
		if (!w->used || !w->frame || !w->frame_dirty)
			continue;
		/* Aero glow polarity: sample the wallpaper under the title bar before rendering it, with
		 * hysteresis against the window's own previous decision (per-window, not per-frame — a
		 * dragged window keeps sampling its NEW rect on every dirty re-render). */
		w->ink_dark = (int8_t) nw_backdrop_wants_dark_ink(w->x, w->y, frame_w(w), NW_TITLEBAR_H, w->ink_dark);
		struct nw_surface fs;
		fs.px = w->frame; fs.w = frame_w(w); fs.h = frame_h(w); fs.stride = frame_w(w);
		nw_surface_noclip(&fs);
		draw_window_to(&fs, w, (i == s->focus), 0, 0);   /* window-local: origin (0,0) */
		w->frame_dirty = 0;
		w->frame_gen = ++g_render_gen;                   /* mark the content texture stale for the GPU */
	}
}

/* Fill bdc->bd over the window rect (x,y,w,h) with a blurred copy of `back` beneath it.
 * Samples a cache_rect (window expanded by the blur radius, clamped to the scissor+screen),
 * downsamples it into bdc->lo, blurs the small copy with nw_blur_rect, then bilinearly
 * upscales the window-rect portion back into bdc->bd. Returns 1 if bd was filled, else 0
 * (region empty, or lo scratch too small -> caller composites without a backdrop). */
/* Fill bdc->bd with the blurred backdrop under window `w`'s rect. Decides between four sources:
 *  - reuse:  the per-window cache is valid for this exact rect (not dirty/moved) -> upsample it;
 *  - slide:  during a drag between forced rebuilds (allow_slide) -> reuse the last blur, resampled
 *            at the moved rect's size (approximate, snaps back on the next forced rebuild);
 *  - fresh:  allow_fresh -> downsample+blur the live scene and persist into the cache (*did_fresh=1);
 *  - stale:  a fresh rebuild is needed but not allowed (budget exhausted) and a cache exists ->
 *            reuse the stale cache rather than render empty.
 * Returns 1 if bd was filled; 0 only when the region is empty/too small OR a fresh build was needed,
 * not allowed, and there is no cache to fall back on (caller then composites with no blur this frame).
 *
 * NOTE (cost-only concession, per plan): the blur is computed over the full cache_rect even when the
 * window is partly occluded by a window above it. nw_rect_visible_band() exists to shrink that work,
 * but wiring it here would desync the cache rect used for reuse matching; left as a future refinement. */
static int build_backdrop_ex(const struct nw_surface *back, const struct nw_backdrop_ctx *bdc,
                             struct nw_window *w, int x, int y, int fw, int fh,
                             int force_rebuild, int allow_slide, int allow_fresh, int *did_fresh)
{
	if (did_fresh) *did_fresh = 0;
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
	int have_cache = w->bd_blur && w->bd_lw > 0;
	if (!reuse) {
		if (allow_slide && have_cache) {
			lw = w->bd_lw; lh = w->bd_lh;       /* drag: reuse last blur, resampled to the new size */
		} else if (allow_fresh) {
			nw_downsample_box(back->px + (long) want.y * back->stride + want.x,
			                  lw * f, lh * f, back->stride, bdc->lo, f);
			struct nw_surface lo = { bdc->lo, lw, lh, lw, 0,0,0,0 };
			nw_surface_noclip(&lo);
			nw_blur_rect(&lo, 0, 0, lw, lh, bdc->radius / f > 0 ? bdc->radius / f : 1, bdc->passes);
			if (w->bd_blur) {                       /* persist into the per-window cache */
				for (int i = 0; i < lw * lh; i++) w->bd_blur[i] = bdc->lo[i];
				w->bd_lw = lw; w->bd_lh = lh; w->bd_rect = want; w->bd_dirty = 0;
			}
			if (did_fresh) *did_fresh = 1;
		} else if (have_cache) {
			lw = w->bd_lw; lh = w->bd_lh;       /* budget exhausted: reuse the stale cache, never blank */
		} else {
			return 0;                            /* nothing to draw from -> compose with no blur */
		}
	}
	const uint32_t *lo_src = reuse ? w->bd_blur : (w->bd_blur ? w->bd_blur : bdc->lo);
	nw_upsample_bilinear(lo_src, lw, lh,
	                     bdc->bd->px + (long) want.y * bdc->bd->stride + want.x,
	                     want.w - (want.w % f), want.h - (want.h % f), bdc->bd->stride);
	return 1;
}

static void draw_chrome(const struct nw_server *s, const struct nw_surface *back, int dim);

void nw_compose_scene(const struct nw_server *s, const struct nw_surface *back,
                      const struct nw_surface *scratch, const struct nw_surface *wall,
                      const struct nw_backdrop_ctx *bdc)
{
	if (wall) nw_blit(back, 0, 0, wall, 0, 0, back->w, back->h);
	else      nw_fill_rect(back, 0, 0, back->w, back->h, 0x1e2a3a);

	int budget = bdc ? bdc->rebuild_budget : 0;    /* non-priority fresh blur rebuilds left this frame */
	for (int z = 0; z < s->zn; z++) {
		int idx = s->zorder[z];
		const struct nw_window *w = &s->win[idx];
		if (!w->used || w->minimized) continue;       /* minimized windows live only on the taskbar */
		int fw = frame_w(w), fh = frame_h(w), focused = (idx == s->focus);
		int dark = (w->title[0] == '\x01');
		int alpha = dark ? DARK_ALPHA : WIN_ALPHA;     /* compiled fallback */
		if (bdc && bdc->win_alpha > 0)                 /* runtime override from settings */
			alpha = dark ? bdc->dark_alpha : bdc->win_alpha;
		int force = 0;
		if (bdc && idx == bdc->drag_win)
			force = (bdc->frame_ctr % NW_BD_FASTDRAG_N) == 0;
		int allow_slide = bdc && idx == bdc->drag_win && !force;
		/* The active window + all non-NORMAL (menu/popup/dialog/tooltip) windows always rebuild
		 * fresh; background NORMAL windows draw fresh rebuilds from a per-frame budget so a screen
		 * full of glass stays responsive (they fall back to their last blur when the budget runs out). */
		int prio = (idx == s->focus) || (w->type != NW_WIN_NORMAL);
		int allow_fresh = prio || budget > 0;
		const struct nw_surface *bd = 0;
		if (bdc && bdc->bd && w->glass) {
			int did_fresh = 0;
			if (build_backdrop_ex(back, bdc, (struct nw_window *) w, w->x, w->y, fw, fh,
			                      force, allow_slide, allow_fresh, &did_fresh))
				bd = bdc->bd;
			if (did_fresh && !prio && budget > 0) budget--;
		}
		if (s_shadow && (w->frame || scratch)) {  /* soft drop shadow cast on the layers below */
			for (int k = 0; k < 4; k++)
				nw_fill_round(back, w->x - k, w->y - k + 5, fw + 2 * k, fh + 2 * k,
				              s_radius + k, 0x000000, 26);
		}
		if (w->frame) {                          /* cached frame: composite window-local source */
			struct nw_surface fs;
			fs.px = w->frame; fs.w = fw; fs.h = fh; fs.stride = fw;
			nw_surface_noclip(&fs);
			composite_round(back, &fs, w->x, w->y, fw, fh, s_radius, alpha, w->x, w->y, bd);
			nw_stroke_round(back, w->x, w->y, fw, fh, s_radius, COL_BORDER, 150);
		} else if (scratch) {                    /* screen-space scratch: render live + composite */
			draw_window_to(scratch, w, focused, w->x, w->y);
			composite_round(back, scratch, w->x, w->y, fw, fh, s_radius, alpha, 0, 0, bd);
			nw_stroke_round(back, w->x, w->y, fw, fh, s_radius, COL_BORDER, 150);
		} else {
			draw_window_to(back, w, focused, w->x, w->y);     /* simple/host path: opaque, square */
		}
	}
	draw_chrome(s, back, 1);
}

/* Chrome = everything above the windows: the top panel, taskbar, open dropdown, and the Run/Auth
 * modals. Split out so the GL compositor (nw_compose_gl.c) can render it into a transparent overlay
 * (dim=0: the GPU draws the modal desktop-dim itself as a full-screen quad, so the overlay stays
 * transparent outside the actual chrome). The CPU scene path calls it with dim=1. */
static void draw_chrome(const struct nw_server *s, const struct nw_surface *back, int dim)
{
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

	if (s->auth_open) {                           /* the system authentication dialog, above all */
		if (dim) nw_blend_rect(back, 0, 0, s->screen_w, s->screen_h, 0x000000, 130); /* dim desktop */
		int x, y, w, h; nw_auth_rect(s, &x, &y, &w, &h);
		nw_fill_round(back, x + 6, y + 8, w, h, 16, 0x000000, 70);            /* drop shadow */
		nw_fill_round(back, x, y, w, h, 16, 0xf4f6fa, 255);                   /* panel body */
		nw_stroke_round(back, x, y, w, h, 16, 0xffffff, 50);
		/* a small accent lock badge + title */
		nw_fill_round(back, x + 20, y + 18, 26, 26, 8, 0x0a84ff, 255);
		nw_fill_round(back, x + 28, y + 26, 10, 12, 3, 0xffffff, 255);
		nw_text(back, x + 58, y + 20, "Authentication Required", 0x172130);
		/* message: which command wants to run as administrator */
		const char *cmd = s->auth_cmd, *base = cmd;
		for (const char *p = cmd; *p; p++) if (*p == '/') base = p + 1;
		char msg[NW_RUN_MAX + 48]; int m = 0;
		for (const char *p = "\""; *p && m < (int) sizeof msg - 1; p++) msg[m++] = *p;
		for (const char *p = base; *p && m < (int) sizeof msg - 1; p++) msg[m++] = *p;
		for (const char *p = "\" wants to make changes."; *p && m < (int) sizeof msg - 1; p++) msg[m++] = *p;
		msg[m] = 0;
		nw_text(back, x + 20, y + 56, msg, 0x3a3a45);
		nw_text(back, x + 20, y + 76, "Enter an administrator password to allow this:", 0x6a6a75);
		/* password field: a white box of dots + a caret */
		int fx = x + 20, fy = y + 98, fw = w - 40, fh = 28;
		nw_fill_round(back, fx, fy, fw, fh, 7, 0xffffff, 255);
		nw_stroke_round(back, fx, fy, fw, fh, 7, 0x0a84ff, 220);
		int dx = fx + 10, dcy = fy + fh / 2;
		for (int i = 0; i < s->auth_passlen; i++) { nw_fill_round(back, dx, dcy - 3, 7, 7, 4, 0x33333a, 255); dx += 13; }
		nw_blend_rect(back, dx + 1, fy + 6, 2, fh - 12, 0x808088, 255);
		/* buttons: Authenticate (default, accent) + Cancel */
		for (int b = 0; b < 2; b++) {
			int bx, by, bw, bh; nw_auth_btn_rect(s, b, &bx, &by, &bw, &bh);
			int hov = (s->auth_hover == b);
			uint32_t fill = (b == 0) ? (hov ? 0x0060df : 0x0a84ff) : (hov ? 0xdbe1ea : 0xeef2f8);
			uint32_t ink  = (b == 0) ? 0xffffff : 0x1c1c1e;
			nw_fill_round(back, bx, by, bw, bh, 8, fill, 255);
			const char *lbl = (b == 0) ? "Authenticate" : "Cancel";
			int lw = nw_text_w(lbl);
			nw_text(back, bx + (bw - lw) / 2, by + (bh - NW_FONT_H) / 2, lbl, ink);
		}
	}
}

/* Render ONLY the chrome (panel/taskbar/dropdown/modals) into `overlay`, cleared to the transparent
 * key colour 0x000000 (the GL compositor keys that out). Black — NOT magenta — is deliberate: the
 * window drop-shadows draw_chrome paints blend against this key colour, and only a BLACK key lets a
 * shadow (dark over black → still ~black) fall inside the key threshold and vanish; a magenta key
 * turns those shadow-over-key blends into visible magenta smears across the window footprints. The
 * GPU composites the windows + glass + blur itself and draws this overlay last; the modal desktop-dim
 * is a GPU quad, so it is omitted here (dim=0). */
void nw_compose_chrome(const struct nw_server *s, const struct nw_surface *overlay)
{
	nw_fill_rect(overlay, 0, 0, overlay->w, overlay->h, 0x000000);   /* transparent key (black) */
	draw_chrome(s, overlay, 0);
}

void nw_compose(const struct nw_server *s, const struct nw_surface *back)
{
	nw_compose_scene(s, back, 0, 0, 0);
	nw_draw_cursor(back, s->cursor_x, s->cursor_y);
}
