/*
 * nw_compose.c — render the compositor state into a backbuffer (see nw_compose.h). Pure.
 */
#include "nw_compose.h"

/* Palette (0x00RRGGBB). */
#define COL_DESKTOP   0x1e2a3a
#define COL_TITLE_FG  0xffffff
#define COL_TITLE_ON  0x3a78c0   /* focused window title bar */
#define COL_TITLE_OFF 0x586070   /* unfocused */
#define COL_BORDER    0x101418
#define COL_CLOSE     0xc04040
#define COL_CLOSE_X   0xffffff
#define COL_CURSOR_FG 0x000000   /* outline */
#define COL_CURSOR_BG 0xffffff   /* fill    */

/* Classic 11x16 arrow cursor: 'X' outline, '.' fill, ' ' transparent. */
static const char *const CURSOR[16] = {
	"X          ",
	"XX         ",
	"X.X        ",
	"X..X       ",
	"X...X      ",
	"X....X     ",
	"X.....X    ",
	"X......X   ",
	"X.......X  ",
	"X........X ",
	"X.....XXXXX",
	"X..X..X    ",
	"X.X X..X   ",
	"XX  X..X   ",
	"X    X..X  ",
	"     XXX   "
};

static void draw_window(const struct nw_window *w, int focused, const struct nw_surface *back)
{
	int fw = w->cw + 2 * NW_BORDER;
	int fh = NW_TITLEBAR_H + w->ch + NW_BORDER;

	/* frame: border fill, then title bar, then content */
	nw_fill_rect(back, w->x, w->y, fw, fh, COL_BORDER);
	nw_fill_rect(back, w->x, w->y, fw, NW_TITLEBAR_H, focused ? COL_TITLE_ON : COL_TITLE_OFF);
	nw_draw_text(back, w->x + 6, w->y + (NW_TITLEBAR_H - NW_FONT_H) / 2, w->title,
	             COL_TITLE_FG, focused ? COL_TITLE_ON : COL_TITLE_OFF);

	/* close box (top-right of the title bar) with an X */
	int cbx = w->x + fw - NW_BORDER - NW_CLOSE - 2;
	int cby = w->y + (NW_TITLEBAR_H - NW_CLOSE) / 2;
	nw_fill_rect(back, cbx, cby, NW_CLOSE, NW_CLOSE, COL_CLOSE);
	for (int i = 2; i < NW_CLOSE - 2; i++) {
		nw_put_pixel(back, cbx + i, cby + i, COL_CLOSE_X);
		nw_put_pixel(back, cbx + (NW_CLOSE - 1 - i), cby + i, COL_CLOSE_X);
	}

	/* content */
	int cox = w->x + NW_BORDER, coy = w->y + NW_TITLEBAR_H;
	if (w->buf) {
		struct nw_surface src;
		src.px = w->buf; src.w = w->cw; src.h = w->ch; src.stride = w->cw;
		nw_blit(back, cox, coy, &src, 0, 0, w->cw, w->ch);
	} else {
		nw_fill_rect(back, cox, coy, w->cw, w->ch, 0x000000);
	}
}

void nw_draw_cursor(const struct nw_surface *dst, int x, int y)
{
	for (int r = 0; r < NW_CURSOR_H; r++) {
		const char *row = CURSOR[r];
		for (int c = 0; row[c]; c++) {
			if (row[c] == 'X')      nw_put_pixel(dst, x + c, y + r, COL_CURSOR_FG);
			else if (row[c] == '.') nw_put_pixel(dst, x + c, y + r, COL_CURSOR_BG);
		}
	}
}

#define COL_PANEL    0x1a2028
#define COL_BTN      0x3a4450
#define COL_BTN_SD   0x8a4040     /* shutdown button: reddish */
#define COL_BTN_TEXT 0xffffff

static void draw_button(const struct nw_server *s, int id, const char *label,
                        uint32_t bg, const struct nw_surface *back)
{
	int x, y, w, h;
	nw_panel_button_rect(s, id, &x, &y, &w, &h);
	nw_fill_rect(back, x, y, w, h, bg);
	nw_draw_text(back, x + 6, y + (h - NW_FONT_H) / 2, label, COL_BTN_TEXT, bg);
}

static void draw_panel(const struct nw_server *s, const struct nw_surface *back)
{
	nw_fill_rect(back, 0, 0, back->w, NW_PANEL_H, COL_PANEL);
	draw_button(s, NW_PANEL_SHUTDOWN, "Shutdown", COL_BTN_SD, back);
	draw_button(s, NW_PANEL_QUIT, "Quit", COL_BTN, back);
}

void nw_compose_scene(const struct nw_server *s, const struct nw_surface *back)
{
	nw_fill_rect(back, 0, 0, back->w, back->h, COL_DESKTOP);
	for (int z = 0; z < s->zn; z++) {            /* back to front */
		int idx = s->zorder[z];
		if (s->win[idx].used)
			draw_window(&s->win[idx], idx == s->focus, back);
	}
	draw_panel(s, back);                         /* menu bar always on top */
}

void nw_compose(const struct nw_server *s, const struct nw_surface *back)
{
	nw_compose_scene(s, back);
	nw_draw_cursor(back, s->cursor_x, s->cursor_y);
}
