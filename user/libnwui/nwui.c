/*
 * nwui.c — toolkit lifecycle + the event loop, over libnw. The only part doing I/O:
 * nwui_open() connects + creates the window, nwui_run() pumps nw_next_event -> nwui_dispatch
 * (core) -> nwui_render (paint) -> nw_commit (damage). The app never touches libnw directly.
 */
#include "nwui_core.h"
#include "libnw.h"
#include <stdlib.h>

struct nwui_io { nw_display *d; nw_win *win; };

nwui *nwui_open(const char *title, int w, int h)
{
	nw_display *d = nw_connect();
	if (!d)
		return 0;
	nw_win *win = nw_create_window(d, w, h, title);
	if (!win)
		return 0;
	nwui *u = (nwui *) malloc(sizeof *u);
	if (!u)
		return 0;
	nwui_init(u);
	struct nwui_io *io = (struct nwui_io *) malloc(sizeof *io);
	if (!io) { free(u); return 0; }
	io->d = d; io->win = win;
	u->io = io;
	u->win_w = nw_win_width(win);
	u->win_h = nw_win_height(win);
	return u;
}

static void paint(nwui *u)
{
	struct nwui_io *io = (struct nwui_io *) u->io;
	struct nw_surface s;
	nw_win_surface(io->win, &s);
	int x, y, w, h;
	if (nwui_render(u, &s, &x, &y, &w, &h))
		nw_commit(io->win, x, y, w, h);
}

void nwui_run(nwui *u)
{
	struct nwui_io *io = (struct nwui_io *) u->io;
	u->layout_dirty = 1;
	paint(u);                                  /* first frame */
	struct nw_event ev;
	for (;;) {
		int r = nw_next_event(io->d, &ev, -1);
		if (r < 0)
			break;                             /* compositor gone */
		if (r == 0)
			continue;
		if (!nwui_dispatch(u, &ev))            /* CLOSE */
			break;
		if (u->clip_set) { nw_set_clipboard(io->d, u->clip_buf, u->clip_len); u->clip_set = 0; }
		if (u->clip_get) { nw_get_clipboard(io->d); u->clip_get = 0; }
		paint(u);
	}
}
