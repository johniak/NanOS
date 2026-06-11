/*
 * nwterm.c — NanoOS "Terminal" demo: a dark NanWM client showing a neofetch-style splash with
 * the NanoOS mark, in the spirit of the design mockup. A raw libnw client (it draws its own
 * dark buffer); the leading 0x01 in the title asks the compositor for the dark window material.
 */
#include <stdint.h>
#include "libnw.h"
#include "nw_gfx.h"

#define WIN_W 470
#define WIN_H 270
#define BG    0x000f121f   /* terminal ink-dark (matches the compositor's dark material) */
#define FG    0x00dffbff
#define DIM   0x008c9aaf
#define PROMPT 0x0063e6be

/* A small NanoOS "N" mark: two gradient bars + a diagonal. */
static void mark(const struct nw_surface *s, int x, int y, int sz)
{
	int bw = sz / 3;
	nw_vgrad_rect(s, x, y, bw, sz, 0x12a8f4, 0x7d3ff2);
	nw_vgrad_rect(s, x + sz - bw, y, bw, sz, 0xff9d00, 0x6fd033);
	for (int i = 0; i < sz; i++)
		nw_blend_rect(s, x + i * (sz - bw) / sz, y + i - bw / 2, bw, bw, 0x12a8f4, 255);
}

static void render(nw_win *win)
{
	struct nw_surface s;
	nw_win_surface(win, &s);
	nw_fill_rect(&s, 0, 0, s.w, s.h, BG);

	mark(&s, 24, 30, 64);

	int x = 150, y = 24, lh = NW_FONT_H + 4;
	nw_text(&s, x, y, "Welcome to NanoOS Terminal", DIM); y += lh + 4;
	x = nw_text(&s, x, y, "nano@nanobook", PROMPT);
	nw_text(&s, x, y, ":~$ neofetch", FG); y += lh;
	const char *info[] = {
		"OS:     NanoOS 1.0.0 x86_64", "Kernel: 1.0.0-nano", "Shell:  nano-shell 1.0",
		"DE:     Nano Desktop", "WM:     Nano Window Manager", "Theme:  Nano Light",
		"Icons:  Nano Colorful", 0
	};
	for (int i = 0; info[i]; i++) { nw_text(&s, x, y, info[i], FG); y += lh; }
	y += 4;
	x = nw_text(&s, 150, y, "nano@nanobook", PROMPT);
	nw_text(&s, x, y, ":~$ ", FG);
	nw_fill_rect(&s, x + 4 * NW_FONT_W, y, NW_FONT_W, NW_FONT_H, DIM);   /* block cursor */

	nw_commit(win, 0, 0, s.w, s.h);
}

int main(void)
{
	nw_display *d = nw_connect();
	if (!d)
		return 1;
	nw_win *win = nw_create_window(d, WIN_W, WIN_H, "\x01" "Terminal");  /* 0x01 -> dark frame */
	if (!win)
		return 1;
	render(win);

	struct nw_event ev;
	for (;;) {
		int r = nw_next_event(d, &ev, -1);
		if (r < 0) break;
		if (r == 0) continue;
		if (ev.type == NW_EV_CONFIGURE) render(win);
		else if (ev.type == NW_EV_CLOSE) return 0;
	}
	return 0;
}
