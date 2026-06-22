/*
 * nwui.c — toolkit lifecycle + the event loop, over libnw. The only part doing I/O:
 * nwui_open() connects + creates the window, nwui_run() pumps nw_next_event -> nwui_dispatch
 * (core) -> nwui_render (paint) -> nw_commit (damage). The app never touches libnw directly.
 */
#include "nwui_core.h"
#include "libnw.h"
#include <stdlib.h>
#include <time.h>

struct nwui_io { nw_display *d; nw_win *win; };

/* Milliseconds on the monotonic clock — fed to the core so it can time double-clicks. */
static int now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void nwui_spawn(nwui *u, const char *cmd)
{
	struct nwui_io *io = (struct nwui_io *) u->io;
	nw_spawn(io->d, cmd);
}

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
	if (u->nappmenu > 0) {                      /* publish the app menu to the global bar */
		char spec[512];
		nwui_menu_encode(u, spec, sizeof spec);
		nw_set_menu(io->d, spec);
	}
	u->layout_dirty = 1;
	paint(u);                                  /* first frame */
	struct nw_event ev;
	for (;;) {
		int r = nw_next_event(io->d, &ev, -1); /* block until something happens */
		if (r < 0)
			break;                             /* compositor gone */
		if (r == 0)
			continue;
		/* COALESCE: a scrollbar drag (or fast typing) floods pointer/key events. Process every
		 * one that is already queued, then paint+commit ONCE — repainting after each event would
		 * push a full list repaint through the pipe per mouse-move and lag badly. This mirrors the
		 * compositor's own coalesce-then-render-once loop. */
		int alive = 1;
		do {
			if (ev.type == NW_EV_MENU) {       /* a global-menu item was chosen */
				nwui_menu_dispatch(u, ev.menu, ev.item);
			} else {
				/* A resize must realloc the client draw buffer to the new size BEFORE we relayout
				 * and repaint at it — otherwise paint() keeps drawing into the old (smaller) buffer
				 * and everything past the old bounds vanishes when the window is enlarged. */
				if (ev.type == NW_EV_CONFIGURE)
					nw_win_resize(io->win, ev.x, ev.y);
				u->now_ms = now_ms();          /* stamp time so the core can detect double-clicks */
				if (!nwui_dispatch(u, &ev)) { alive = 0; break; }   /* CLOSE */
				if (u->clip_set) { nw_set_clipboard(io->d, u->clip_buf, u->clip_len); u->clip_set = 0; }
				if (u->clip_get) { nw_get_clipboard(io->d); u->clip_get = 0; }
			}
		} while (nw_next_event(io->d, &ev, 0) > 0);   /* drain the rest, non-blocking */
		if (!alive)
			break;
		paint(u);
	}
}
