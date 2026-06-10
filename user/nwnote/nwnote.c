/*
 * nwnote.c — a tiny NanWM demo client: a text note. Connects to the compositor via libnw,
 * draws typed text into its window, and participates in the macOS-style clipboard:
 *   Super+C / Super+X -> the compositor sends NW_EV_COPY; we offer our text as the clipboard
 *                        (and on cut, clear it),
 *   Super+V           -> NW_EV_PASTE delivers the clipboard text; we append it.
 * Two instances are spawned by nwm, so copy-in-one / paste-in-the-other demonstrates the
 * server-side clipboard end to end. This IS a GetMessage-style event loop.
 */
#include <stdint.h>
#include <string.h>
#include "libnw.h"
#include "nw_gfx.h"

#define WIN_W 360
#define WIN_H 220
#define BG    0x00f4f4ec   /* paper */
#define FG    0x00101014   /* ink   */
#define CURSOR 0x00c04040

static char  text[2048];
static int   tlen;

static void render(nw_win *win)
{
	struct nw_surface s;
	nw_win_surface(win, &s);
	nw_fill_rect(&s, 0, 0, s.w, s.h, BG);

	int x = 4, y = 4;
	int maxcols = (s.w - 8) / NW_FONT_W;
	int col = 0;
	for (int i = 0; i < tlen; i++) {
		char c = text[i];
		if (c == '\n') { x = 4; y += NW_FONT_H; col = 0; continue; }
		nw_draw_char(&s, x, y, (unsigned char) c, FG, BG);
		x += NW_FONT_W; col++;
		if (col >= maxcols) { x = 4; y += NW_FONT_H; col = 0; }
	}
	/* text cursor */
	nw_fill_rect(&s, x, y, 2, NW_FONT_H, CURSOR);

	nw_commit(win, 0, 0, s.w, s.h);
}

int main(void)
{
	nw_display *d = nw_connect();
	if (!d)
		return 1;
	nw_win *win = nw_create_window(d, WIN_W, WIN_H, "note");
	if (!win)
		return 1;
	render(win);

	struct nw_event ev;
	for (;;) {
		int r = nw_next_event(d, &ev, -1);
		if (r < 0)
			break;                       /* compositor gone */
		if (r == 0)
			continue;
		switch (ev.type) {
		case NW_EV_KEY:
			if (!ev.down || ev.ch == 0)
				break;
			if (ev.ch == 8) {            /* backspace */
				if (tlen > 0) tlen--;
			} else if (ev.ch >= 32 || ev.ch == '\n' || ev.ch == '\t') {
				if (tlen < (int) sizeof text - 1) text[tlen++] = ev.ch;
			}
			render(win);
			break;
		case NW_EV_COPY:
			nw_set_clipboard(d, text, tlen);
			if (ev.cut) { tlen = 0; render(win); }
			break;
		case NW_EV_PASTE:
			for (int i = 0; i < ev.text_len && tlen < (int) sizeof text - 1; i++)
				text[tlen++] = ev.text[i];
			render(win);
			break;
		case NW_EV_CLOSE:
			return 0;                    /* exit -> our pipes close -> compositor drops us */
		default:
			break;
		}
	}
	return 0;
}
