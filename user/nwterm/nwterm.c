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

int setsid(void);
int getpid(void);
int ioctl(int fd, unsigned long request, ...);
#define NWT_TIOCSPGRP   0x5410
#define NWT_TIOCSWINSZ  0x5414           /* tell the pty its size so vim/bash size their screen */
struct nwt_winsize { unsigned short row, col, xpixel, ypixel; };
#include "nw_gfx.h"
#include "vt.h"

#define CW NW_FONT_W
#define CH NW_FONT_H

static vt        T;

/* Push the current grid geometry to the pty (TIOCSWINSZ), so a program reading TIOCGWINSZ on
 * pts0 sees the real terminal size instead of the kernel's 80x24 default — otherwise vim renders
 * at 80 columns inside our narrower grid and the text wraps/overflows the window. */
static void set_pty_winsize(int fd)
{
	struct nwt_winsize ws = { (unsigned short) T.rows, (unsigned short) T.cols, 0, 0 };
	ioctl(fd, NWT_TIOCSWINSZ, &ws);
}
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
	if (g_master >= 0) set_pty_winsize(g_master);   /* keep the pty's size in step with the window */
	struct nw_surface s; nw_win_surface(g_win, &s);
	nw_fill_rect(&s, 0, 0, s.w, s.h, vt_pal(0));        /* repaint background, then all rows */
	render();
	nw_commit(g_win, 0, 0, s.w, s.h);
}

static int spawn_shell(void)
{
	int master = open("/dev/ptmx", O_RDWR);
	if (master < 0) return -1;
	set_pty_winsize(master);             /* size the pty to our grid BEFORE the shell/vim starts */
	/* Leave the pty in the kernel's default COOKED mode (ICANON|ECHO|ISIG, ICRNL, OPOST|ONLCR):
	 * a terminal emulator must NOT force raw — that is the shell's job. With cooked+echo the kernel
	 * line discipline echoes each typed character immediately (so input shows as you type) and the
	 * shell still reads whole lines; an interactive shell that wants raw editing flips it itself. */
	int pid = fork();
	if (pid == 0) {
		/* Own session with pts0 as its job-control terminal, isolated from the console — else a
		 * job-control shell (bash) grabs the console and writes to the screen, not the window.
		 * No TIOCSCTTY in the kernel, so make pts0's foreground group our new session group; that
		 * keeps bash's job-control gate (tcgetpgrp == our pgid) satisfied so it doesn't stop. */
		setsid();
		int s = open("/dev/pts0", O_RDWR);
		int pg = getpid();
		ioctl(s, NWT_TIOCSPGRP, &pg);
		dup2(s, 0); dup2(s, 1); dup2(s, 2);
		if (s > 2) close(s);
		close(master); close(3); close(4);             /* drop the compositor pipes in the shell */
		/* Keep this in step with PID 1's baseline env (kernel/Exec.cpp): the windowed terminal
		 * must give programs the same environment as the boot console, or they behave differently
		 * here. In particular VIMRUNTIME + VIMINIT stop vim sourcing its missing defaults.vim
		 * ("E1187: Failed to source defaults.vim"). nwm only forwards NW_DISPLAY to its clients,
		 * so these are set explicitly rather than inherited. */
		char *envp[] = { (char *) "TERM=xterm-256color",
		                 (char *) "TERMINFO=/disks/main/nanos/share/terminfo",
		                 (char *) "PATH=/disks/main/nanos/bin:/disks/main/bin",
		                 (char *) "HOME=/disks/main",
		                 (char *) "VIMRUNTIME=/disks/main/apps/vim/runtime",
		                 (char *) "VIMINIT=set nocompatible backspace=indent,eol,start hlsearch incsearch ruler showcmd wildmenu",
		                 0 };
		/* the login shell from the account database (pw_shell, e.g. bash); nsh if absent */
		struct passwd *pw = getpwuid(getuid());
		const char *shell = (pw && pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell
		                  : "/disks/main/nanos/bin/nsh.nxe";
		const char *base = strrchr(shell, '/'); base = base ? base + 1 : shell;
		static char name0[64];
		{ int i = 0; while (base[i] && i < (int) sizeof name0 - 1) { name0[i] = base[i]; i++; }
		  name0[i] = 0; if (i >= 4 && strcmp(name0 + i - 4, ".nxe") == 0) name0[i - 4] = 0; }
		execve(shell, (char *[]){ name0, 0 }, envp);
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
			/* A bigger buffer coalesces a burst (e.g. a full vim redraw) into one vt_feed +
			 * one render/commit instead of several 1 KiB chunks each triggering a partial commit. */
			unsigned char ob[16384];
			int n = read(g_master, ob, sizeof ob);
			if (n <= 0) return 0;                        /* shell exited */
			vt_feed(&T, ob, n);
			render();
		}
	}
}
