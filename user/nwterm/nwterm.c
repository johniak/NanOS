/*
 * nwterm — the real NanoOS Terminal: a NanWM window running `nsh` on a pty.
 *
 * Unlike the framebuffer terminal (nterm), this is a libnw client: it draws the VT grid into its
 * window buffer and gets keystrokes as NanWM KEY events. The shared VT engine (vt.c) parses the
 * shell's output (xterm subset, ANSI colours). The event loop polls TWO fds — the compositor's
 * event pipe (via nw_event_fd) and the pty master — so window input and shell output interleave.
 * Resizing the window reflows the grid (vt_resize).
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <pwd.h>
#include <string.h>
#include <sys/termios.h>
#include "libnw.h"
#include "nw_gfx.h"
#include "vt.h"

#define CW NW_FONT_W
#define CH NW_FONT_H

static vt        T;
static nw_win   *g_win;
static int       g_master;
static int       g_ctrl;
static int       g_pcx, g_pcy;        /* last drawn cursor cell */

/* ---- rendering: VT grid -> window surface ---- */
static void draw_row(const struct nw_surface *s, int r)
{
	for (int c = 0; c < T.cols; c++) {
		vt_cell *cell = &T.grid[r][c];
		nw_draw_char(s, c * CW, r * CH, cell->ch, vt_pal(cell->fg), vt_pal(cell->bg));
	}
}

static void render(void)
{
	struct nw_surface s;
	nw_win_surface(g_win, &s);
	int y0 = -1, y1 = -1;
	for (int r = 0; r < T.rows; r++) {
		if (!T.dirty[r] && r != T.cy && r != g_pcy)
			continue;
		draw_row(&s, r);
		T.dirty[r] = 0;
		if (y0 < 0) y0 = r;
		y1 = r;
	}
	/* block cursor: invert the cell under it */
	if (T.cx < T.cols && T.cy < T.rows) {
		vt_cell *cell = &T.grid[T.cy][T.cx];
		nw_fill_rect(&s, T.cx * CW, T.cy * CH, CW, CH, vt_pal(cell->fg));
		nw_draw_char(&s, T.cx * CW, T.cy * CH, cell->ch, vt_pal(cell->bg), vt_pal(cell->fg));
	}
	g_pcx = T.cx; g_pcy = T.cy;
	if (y0 >= 0) nw_commit(g_win, 0, y0 * CH, s.w, (y1 - y0 + 1) * CH);
}

/* ---- NanWM KEY event -> bytes to the pty ---- */
static void key(const struct nw_event *ev)
{
	int sc = ev->code & 0x7f, ext = ev->code & 0x80;
	if (sc == 0x1d) { g_ctrl = ev->down; return; }      /* Ctrl tracked from the scancode */
	if (!ev->down) return;
	if (ext) {                                          /* extended nav keys -> VT sequences */
		const char *seq = 0;
		if (sc == 0x48) seq = "\x1b[A"; else if (sc == 0x50) seq = "\x1b[B";
		else if (sc == 0x4d) seq = "\x1b[C"; else if (sc == 0x4b) seq = "\x1b[D";
		else if (sc == 0x47) seq = "\x1b[H"; else if (sc == 0x4f) seq = "\x1b[F";
		else if (sc == 0x49) seq = "\x1b[5~"; else if (sc == 0x51) seq = "\x1b[6~";
		else if (sc == 0x52) seq = "\x1b[2~"; else if (sc == 0x53) seq = "\x1b[3~";
		if (seq) { int l = 0; while (seq[l]) l++; write(g_master, seq, l); }
		return;
	}
	char ch = ev->ch;                                   /* compositor already applied Shift */
	if (!ch) return;
	if (g_ctrl) {                                       /* Ctrl+A..Z -> 1..26 (e.g. Ctrl+C) */
		if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;
		else if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 1;
	}
	write(g_master, &ch, 1);
}

static void resize_to(int win_w, int win_h)
{
	int cols = win_w / CW, rows = win_h / CH;
	if (cols < 1) cols = 1; if (rows < 1) rows = 1;
	vt_resize(&T, cols, rows);
	struct nw_surface s; nw_win_surface(g_win, &s);
	nw_fill_rect(&s, 0, 0, s.w, s.h, vt_pal(0));        /* repaint background, then all rows */
	render();
	nw_commit(g_win, 0, 0, s.w, s.h);
}

static int spawn_shell(void)
{
	int master = open("/dev/ptmx", O_RDWR);
	if (master < 0) return -1;
	struct termios t;
	tcgetattr(master, &t);
	t.c_lflag &= ~(ICANON | ECHO); t.c_lflag |= ISIG; t.c_oflag = 0; t.c_iflag = ICRNL;
	tcsetattr(master, TCSANOW, &t);
	int pid = fork();
	if (pid == 0) {
		int s = open("/dev/pts0", O_RDWR);
		dup2(s, 0); dup2(s, 1); dup2(s, 2);
		if (s > 2) close(s);
		close(master); close(3); close(4);             /* drop the compositor pipes in the shell */
		char *envp[] = { (char *) "TERM=xterm-256color",
		                 (char *) "TERMINFO=/disks/main/nanos/share/terminfo",
		                 (char *) "PATH=/disks/main/nanos/bin:/disks/main/bin",
		                 (char *) "HOME=/disks/main", 0 };
		/* the native NanOS shell — simple, prompts on a raw pty without bash's job-control setup */
		execve("/disks/main/nanos/bin/nsh.nxe", (char *[]){ (char *) "nsh", 0 }, envp);
		_exit(127);
	}
	return master;
}

int main(void)
{
	nw_display *d = nw_connect();
	if (!d) return 1;
	g_win = nw_create_window(d, 560, 360, "\x01" "Terminal");   /* 0x01 -> dark window material */
	if (!g_win) return 1;
	nw_set_menu(d, "Terminal\x1f" "Close\x1e" "Edit\x1f" "Paste");  /* global menu */
	vt_init(&T, nw_win_width(g_win) / CW, nw_win_height(g_win) / CH);

	g_master = spawn_shell();
	if (g_master < 0) return 1;

	{ struct nw_surface s; nw_win_surface(g_win, &s);
	  nw_fill_rect(&s, 0, 0, s.w, s.h, vt_pal(0)); render(); nw_commit(g_win, 0, 0, s.w, s.h); }

	int efd = nw_event_fd(d);
	for (;;) {
		struct pollfd pf[2];
		pf[0].fd = efd;      pf[0].events = POLLIN; pf[0].revents = 0;
		pf[1].fd = g_master; pf[1].events = POLLIN; pf[1].revents = 0;
		poll(pf, 2, -1);

		if (pf[0].revents & POLLIN) {                   /* window events (keys, resize, close) */
			struct nw_event ev;
			int r;
			while ((r = nw_next_event(d, &ev, 0)) > 0) {
				if (ev.type == NW_EV_KEY)            key(&ev);
				else if (ev.type == NW_EV_CONFIGURE) resize_to(ev.x, ev.y);
				else if (ev.type == NW_EV_CLOSE)     return 0;
				else if (ev.type == NW_EV_MENU) {    /* Terminal>Close / Edit>Paste */
					if (ev.menu == 0) return 0;
					else nw_get_clipboard(d);        /* reply arrives as NW_EV_PASTE */
				} else if (ev.type == NW_EV_PASTE && ev.text) {
					write(g_master, ev.text, ev.text_len);   /* paste into the shell */
				}
			}
			if (r < 0) return 0;                        /* compositor gone */
		}
		if (pf[1].revents & POLLIN) {                   /* shell output -> VT -> render */
			unsigned char ob[1024];
			int n = read(g_master, ob, sizeof ob);
			if (n <= 0) return 0;                        /* shell exited */
			vt_feed(&T, ob, n);
			render();
		}
	}
}
