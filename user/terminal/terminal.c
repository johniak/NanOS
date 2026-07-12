/*
 * terminal — the real Nano OS Terminal: a nanowm window running `nsh` on a pty.
 *
 * Unlike the framebuffer terminal (nterm), this is a libnw client: it draws the VT grid into its
 * window buffer and gets keystrokes as nanowm KEY events. The shared VT engine (vt.c) parses the
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
#include "nwproto.h"    /* NW_STYLE_* (libnw.h doesn't re-export it) */

int setsid(void);
int getpid(void);
int ioctl(int fd, unsigned long request, ...);
#define NWT_TIOCSPGRP   0x5410
#define NWT_TIOCSCTTY   0x540E           /* adopt pts0 as the controlling terminal (so /dev/tty resolves here) */
#define NWT_TIOCSWINSZ  0x5414           /* tell the pty its size so vim/bash size their screen */
struct nwt_winsize { unsigned short row, col, xpixel, ypixel; };
#include "nw_gfx.h"
#include "vt.h"

#define CW NW_FONT_W
#define CH NW_FONT_H

static vt        T;

/* GL glass-client: the top byte is ink alpha. Text and coloured cells are solid ink; the DEFAULT
 * background (palette 0 — confirmed as the VT's default/reset bg: vt_init sets t->bg = 0, and SGR
 * 0/49 both reset t->bg to 0 in vt.c) is a thin dark veil so the glass slab shows through. The CPU
 * fallback ignores the top byte, so this changes nothing there. */
static uint32_t pal_fg(int idx) { return vt_pal(idx) | 0xff000000u; }
static uint32_t pal_bg(int idx)
{
	uint32_t c = vt_pal(idx);
	return idx == 0 ? (c | 0x50000000u) : (c | 0xff000000u);
}

/* The default-bg veil is not an edge-to-edge rect any more: it is a rounded, feather-edged panel
 * (same design language as the libnwui glass panels) and the character grid sits TERM_M px inside
 * it. TERM_M(14) >= TERM_FEATHER(8) keeps the whole grid in the panel's full-alpha interior, so a
 * dirty row's raw per-cell stores of pal_bg(0) are pixel-identical to the veil they overwrite —
 * rows never touch the fade band and need no compositing against it. */
#define TERM_M       14   /* grid inset from the client edges (px) */
#define TERM_R       14   /* veil corner radius */
#define TERM_FEATHER 8    /* veil edge fade width */

static void paint_veil(const struct nw_surface *s)
{
	nw_clear_argb(s, 0, 0, s->w, s->h, 0x00000000u);
	nw_over_round_soft(s, 0, 0, s->w, s->h, TERM_R, pal_bg(0), TERM_FEATHER);
}

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
		nw_draw_char(s, TERM_M + c * CW, TERM_M + r * CH, cell->ch,
		             pal_fg(cell->fg), pal_bg(cell->bg));
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
		/* the cursor block inverts fg/bg, so the wrapper follows the ROLE (background-fill vs
		 * ink), not the lexical cell->fg/cell->bg name: cell->fg here plays the "fills the whole
		 * cell" role (pal_bg), cell->bg plays the "glyph ink" role (pal_fg). */
		nw_fill_rect(&s, TERM_M + T.cx * CW, TERM_M + T.cy * CH, CW, CH, pal_bg(cell->fg));
		nw_draw_char(&s, TERM_M + T.cx * CW, TERM_M + T.cy * CH, cell->ch,
		             pal_fg(cell->bg), pal_bg(cell->fg));
	}
	g_pcx = T.cx; g_pcy = T.cy;
	if (y0 >= 0) nw_commit(g_win, 0, TERM_M + y0 * CH, s.w, (y1 - y0 + 1) * CH);
}

/* ---- nanowm KEY event -> bytes to the pty ---- */
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
	int cols = (win_w - 2 * TERM_M) / CW, rows = (win_h - 2 * TERM_M) / CH;
	if (cols < 1) cols = 1; if (rows < 1) rows = 1;
	vt_resize(&T, cols, rows);
	if (g_master >= 0) set_pty_winsize(g_master);   /* keep the pty's size in step with the window */
	struct nw_surface s; nw_win_surface(g_win, &s);
	paint_veil(&s);                                     /* repaint background, then all rows */
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
		 * TIOCSCTTY makes pts0 this session's controlling terminal (so /dev/tty resolves to the
		 * pty, not a VT), and the foreground group = our new session group keeps bash's job-control
		 * gate (tcgetpgrp == our pgid) satisfied so it doesn't stop. */
		setsid();
		int s = open("/dev/pts0", O_RDWR);
		int pg = getpid();
		ioctl(s, NWT_TIOCSCTTY, (void*) 0);
		ioctl(s, NWT_TIOCSPGRP, &pg);
		dup2(s, 0); dup2(s, 1); dup2(s, 2);
		if (s > 2) close(s);
		close(master); close(3); close(4);             /* drop the compositor pipes in the shell */
		/* Keep this in step with PID 1's baseline env (kernel/Exec.cpp): the windowed terminal
		 * must give programs the same environment as the boot console, or they behave differently
		 * here. In particular VIMRUNTIME + VIMINIT stop vim sourcing its missing defaults.vim
		 * ("E1187: Failed to source defaults.vim"). nwm only forwards NW_DISPLAY to its clients,
		 * so these are set explicitly rather than inherited. */
		/* the login shell + home from the account database (pw_shell/pw_dir, e.g. bash + /users/jan);
		 * fall back to nsh + /disks/main if the entry is missing. */
		struct passwd *pw = getpwuid(getuid());
		const char *shell = (pw && pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell
		                  : "/disks/main/nanos/bin/nsh.nxe";
		const char *home = (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : "/disks/main";

		/* chdir to the user's home BEFORE exec: the shell inherits this as its cwd. Without it the
		 * shell starts in the compositor's directory, which may be the synthetic root or otherwise
		 * un-stat'able, so bash's getcwd() fails ("cannot access parent directories"). */
		if (chdir(home) != 0)
			chdir("/disks/main");

		static char homevar[160];
		{ int i = 5; const char *p = "HOME="; for (int j = 0; j < 5; j++) homevar[j] = p[j];
		  for (const char *h = home; *h && i < (int) sizeof homevar - 1; h++) homevar[i++] = *h;
		  homevar[i] = 0; }

		/* Keep this in step with PID 1's baseline env (kernel/Exec.cpp). HOME comes from the passwd
		 * entry so the shell finds ~/.bashrc; nwm only forwards NW_DISPLAY, so the rest are explicit. */
		char *envp[] = { (char *) "TERM=xterm-256color",
		                 (char *) "TERMINFO=/disks/main/nanos/share/terminfo",
		                 (char *) "PATH=/disks/main/nanos/bin:/disks/main/bin",
		                 homevar,
		                 (char *) "VIMRUNTIME=/disks/main/apps/vim/runtime",
		                 (char *) "VIMINIT=set nocompatible backspace=indent,eol,start hlsearch incsearch ruler showcmd wildmenu",
		                 0 };
		/* Start it as a LOGIN shell (argv[0] prefixed with '-'), like a console login: bash then
		 * sources /etc/profile (PATH + coloured PS1) which sources ~/.bashrc, so the windowed shell
		 * matches the console one. The ".nxe" suffix is stripped from the name. */
		const char *base = strrchr(shell, '/'); base = base ? base + 1 : shell;
		static char name0[64];
		{ name0[0] = '-'; int i = 1;
		  while (base[i - 1] && i < (int) sizeof name0 - 1) { name0[i] = base[i - 1]; i++; }
		  name0[i] = 0; if (i >= 4 && strcmp(name0 + i - 4, ".nxe") == 0) name0[i - 4] = 0; }
		execve(shell, (char *[]){ name0, 0 }, envp);
		execve("/disks/main/nanos/bin/nsh.nxe", (char *[]){ (char *) "-nsh", 0 }, envp);
		_exit(127);
	}
	return master;
}

int main(void)
{
	nw_display *d = nw_connect();
	if (!d) return 1;
	g_win = nw_create_window_style(d, 560, 360, "\x01" "Terminal",   /* \x01 keeps CPU-path dark */
	                               NW_STYLE_GLASS_CLIENT | NW_STYLE_DARK);
	if (!g_win) return 1;
	nw_set_menu(d, "Terminal\x1f" "Close\x1e" "Edit\x1f" "Paste");  /* global menu */
	vt_init(&T, (nw_win_width(g_win) - 2 * TERM_M) / CW,
	            (nw_win_height(g_win) - 2 * TERM_M) / CH);

	g_master = spawn_shell();
	if (g_master < 0) return 1;

	{ struct nw_surface s; nw_win_surface(g_win, &s);
	  paint_veil(&s); render(); nw_commit(g_win, 0, 0, s.w, s.h); }

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
