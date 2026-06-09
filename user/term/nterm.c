/*
 * nterm — NanOS userspace terminal emulator (the "fbterm/st" of NanOS).
 *
 * Owns the framebuffer and is the VT for a shell running on a pty:
 *   - opens /dev/ptmx (master); forks a child that puts /dev/pts0 on stdin/out/err and
 *     execs the shell (the slave is its controlling terminal);
 *   - event loop: poll(master, /dev/input0); keystrokes -> bytes -> write(master);
 *     shell output (master) -> a VT escape-sequence parser -> rasterized to /dev/fb0.
 *
 * Implements the xterm subset shells/TUIs need: CSI cursor moves + absolute position,
 * erase line/display, SGR colours (16 + 256), scroll region, alternate screen, cursor
 * save/restore. Single C file (links libc.ndl); the 8x16 font is font8x16.c.
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <sys/termios.h>

extern const unsigned char nx_font8x16[256][16];
extern void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long off);

/* ---- framebuffer (Linux fbdev) ---- */
struct fb_var { uint32_t xres, yres, xv, yv, xo, yo, bpp, gray; uint32_t pad[40]; };
struct fb_fix { char id[16]; uint32_t smem_start, smem_len, type, type_aux, visual;
	uint16_t xp, yp, yw; uint32_t line_length; uint32_t pad[8]; };
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
int ioctl(int fd, unsigned long request, ...);

static uint8_t* g_fb;
static uint32_t g_pitch, g_xres, g_yres;
#define CW 8
#define CH 16
static int g_cols, g_rows;

/* 256-colour xterm palette -> 0x00RRGGBB. */
static uint32_t pal(int i) {
	static const uint32_t base[16] = {
		0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
		0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff };
	if (i < 0) i = 0;
	if (i < 16) return base[i];
	if (i < 232) { i -= 16; int r = i / 36, g = (i / 6) % 6, b = i % 6;
		int R = r ? r * 40 + 55 : 0, G = g ? g * 40 + 55 : 0, B = b ? b * 40 + 55 : 0;
		return (uint32_t) ((R << 16) | (G << 8) | B); }
	int v = (i - 232) * 10 + 8;
	return (uint32_t) ((v << 16) | (v << 8) | v);
}

/* ---- screen model ---- */
#define MAXC 256
#define MAXR 128
typedef struct { unsigned char ch, fg, bg; } Cell;
static Cell g_grid[MAXR][MAXC];
static int g_cx, g_cy;            // cursor
static int g_fg = 7, g_bg = 0, g_bold, g_rev;
static int g_top, g_bot;          // scroll region [top, bot]
static int g_savecx, g_savecy;

static void cellDraw(int cx, int cy) {
	if (cx < 0 || cy < 0 || cx >= g_cols || cy >= g_rows) return;
	Cell* c = &g_grid[cy][cx];
	int fg = c->fg, bg = c->bg;
	uint32_t fgc = pal(fg), bgc = pal(bg);
	const unsigned char* gl = nx_font8x16[c->ch];
	for (int y = 0; y < CH; y++) {
		uint32_t* row = (uint32_t*) (g_fb + (cy * CH + y) * g_pitch) + cx * CW;
		unsigned char bits = gl[y];
		for (int x = 0; x < CW; x++)
			row[x] = (bits & (0x80 >> x)) ? fgc : bgc;
	}
}

static void redrawAll(void) {
	for (int y = 0; y < g_rows; y++)
		for (int x = 0; x < g_cols; x++)
			cellDraw(x, y);
}

static void clearRegion(int x0, int y0, int x1, int y1) {
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			g_grid[y][x].ch = ' '; g_grid[y][x].fg = g_fg; g_grid[y][x].bg = g_bg;
			cellDraw(x, y);
		}
}

static void scrollUp(void) {
	for (int y = g_top; y < g_bot; y++)
		for (int x = 0; x < g_cols; x++)
			g_grid[y][x] = g_grid[y + 1][x];
	for (int x = 0; x < g_cols; x++) {
		g_grid[g_bot][x].ch = ' '; g_grid[g_bot][x].fg = g_fg; g_grid[g_bot][x].bg = g_bg;
	}
	redrawAll();
}

static void putGlyph(unsigned char ch) {
	if (g_cx >= g_cols) { g_cx = 0; if (++g_cy > g_bot) { g_cy = g_bot; scrollUp(); } }
	Cell* c = &g_grid[g_cy][g_cx];
	c->ch = ch;
	c->fg = g_rev ? g_bg : (g_bold && g_fg < 8 ? g_fg + 8 : g_fg);
	c->bg = g_rev ? g_fg : g_bg;
	cellDraw(g_cx, g_cy);
	g_cx++;
}

static void lineFeed(void) { if (++g_cy > g_bot) { g_cy = g_bot; scrollUp(); } }

/* ---- VT parser ---- */
enum { S_NORM, S_ESC, S_CSI };
static int g_state;
static int g_par[8], g_npar;
static int g_priv;     // CSI '?' private mode

static void sgr(void) {
	if (g_npar == 0) { g_par[0] = 0; g_npar = 1; }
	for (int i = 0; i < g_npar; i++) {
		int p = g_par[i];
		if (p == 0) { g_fg = 7; g_bg = 0; g_bold = 0; g_rev = 0; }
		else if (p == 1) g_bold = 1;
		else if (p == 7) g_rev = 1;
		else if (p == 22) g_bold = 0;
		else if (p == 27) g_rev = 0;
		else if (p >= 30 && p <= 37) g_fg = p - 30;
		else if (p == 39) g_fg = 7;
		else if (p >= 40 && p <= 47) g_bg = p - 40;
		else if (p == 49) g_bg = 0;
		else if (p >= 90 && p <= 97) g_fg = 8 + (p - 90);
		else if (p >= 100 && p <= 107) g_bg = 8 + (p - 100);
		else if (p == 38 && i + 2 < g_npar && g_par[i + 1] == 5) { g_fg = g_par[i + 2]; i += 2; }
		else if (p == 48 && i + 2 < g_npar && g_par[i + 1] == 5) { g_bg = g_par[i + 2]; i += 2; }
	}
}

static int clampr(int v) { return v < 0 ? 0 : (v >= g_rows ? g_rows - 1 : v); }
static int clampc(int v) { return v < 0 ? 0 : (v >= g_cols ? g_cols - 1 : v); }

static void csiFinal(unsigned char f) {
	int a = g_npar > 0 ? g_par[0] : 0;
	int b = g_npar > 1 ? g_par[1] : 0;
	switch (f) {
	case 'H': case 'f':
		g_cy = clampr((a ? a : 1) - 1); g_cx = clampc((b ? b : 1) - 1); break;
	case 'A': g_cy = clampr(g_cy - (a ? a : 1)); break;
	case 'B': g_cy = clampr(g_cy + (a ? a : 1)); break;
	case 'C': g_cx = clampc(g_cx + (a ? a : 1)); break;
	case 'D': g_cx = clampc(g_cx - (a ? a : 1)); break;
	case 'G': g_cx = clampc((a ? a : 1) - 1); break;
	case 'd': g_cy = clampr((a ? a : 1) - 1); break;
	case 'J':
		if (a == 2 || a == 3) clearRegion(0, 0, g_cols - 1, g_rows - 1);
		else if (a == 1) clearRegion(0, 0, g_cx, g_cy);
		else { clearRegion(g_cx, g_cy, g_cols - 1, g_cy);
			if (g_cy + 1 < g_rows) clearRegion(0, g_cy + 1, g_cols - 1, g_rows - 1); }
		break;
	case 'K':
		if (a == 2) clearRegion(0, g_cy, g_cols - 1, g_cy);
		else if (a == 1) clearRegion(0, g_cy, g_cx, g_cy);
		else clearRegion(g_cx, g_cy, g_cols - 1, g_cy);
		break;
	case 'm': sgr(); break;
	case 'r':                                  // DECSTBM scroll region
		g_top = a ? clampr(a - 1) : 0;
		g_bot = b ? clampr(b - 1) : g_rows - 1;
		g_cx = 0; g_cy = g_top; break;
	case 's': g_savecx = g_cx; g_savecy = g_cy; break;
	case 'u': g_cx = g_savecx; g_cy = g_savecy; break;
	case 'h': case 'l':
		if (g_priv && a == 1049) {             // alternate screen: just clear (no scrollback)
			clearRegion(0, 0, g_cols - 1, g_rows - 1); g_cx = g_cy = 0;
		}
		break;                                 // ?25 (cursor visible) etc.: ignored
	}
}

static void feed(unsigned char c) {
	switch (g_state) {
	case S_NORM:
		if (c == 0x1b) { g_state = S_ESC; return; }
		if (c == '\n') { lineFeed(); g_cx = 0; return; }
		if (c == '\r') { g_cx = 0; return; }
		if (c == '\b') { if (g_cx > 0) g_cx--; return; }
		if (c == '\t') { g_cx = clampc((g_cx + 8) & ~7); return; }
		if (c == 7) return;                    // BEL
		if (c >= 32) putGlyph(c);
		return;
	case S_ESC:
		if (c == '[') { g_state = S_CSI; g_npar = 0; g_par[0] = 0; g_priv = 0; return; }
		if (c == '7') { g_savecx = g_cx; g_savecy = g_cy; }
		else if (c == '8') { g_cx = g_savecx; g_cy = g_savecy; }
		g_state = S_NORM;                      // ignore other ESC x
		return;
	case S_CSI:
		if (c == '?') { g_priv = 1; return; }
		if (c >= '0' && c <= '9') {
			if (g_npar == 0) g_npar = 1;
			g_par[g_npar - 1] = g_par[g_npar - 1] * 10 + (c - '0');
			return;
		}
		if (c == ';') { if (g_npar < 8) g_par[g_npar++] = 0; return; }
		csiFinal(c);
		g_state = S_NORM;
		return;
	}
}

/* ---- keyboard (/dev/input0 evdev) -> bytes to the master ---- */
static int g_shift, g_ctrl;
static const char kmap[128] = {
	0,0,'1','2','3','4','5','6','7','8','9','0','-','=','\b',0,
	'q','w','e','r','t','y','u','i','o','p','[',']','\r',0,'a','s',
	'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
	'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0 };
static const char kmapsh[128] = {
	0,0,'!','@','#','$','%','^','&','*','(',')','_','+','\b',0,
	'Q','W','E','R','T','Y','U','I','O','P','{','}','\r',0,'A','S',
	'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
	'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0 };

static void keyEvent(int master, unsigned char code, unsigned char down) {
	int ext = code & 0x80;
	int sc = code & 0x7f;
	if (!ext && (sc == 0x2a || sc == 0x36)) { g_shift = down; return; }
	if ((ext && sc == 0x1d) || (!ext && sc == 0x1d)) { g_ctrl = down; return; }
	if (!down) return;
	char buf[4];
	int n = 0;
	if (ext) {
		const char* seq = 0;
		if (sc == 0x48) seq = "\x1b[A";
		else if (sc == 0x50) seq = "\x1b[B";
		else if (sc == 0x4d) seq = "\x1b[C";
		else if (sc == 0x4b) seq = "\x1b[D";
		if (seq) { write(master, seq, 3); return; }
		return;
	}
	char ch = g_shift ? kmapsh[sc] : kmap[sc];
	if (!ch) return;
	if (g_ctrl && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;        // Ctrl+A..Z -> 1..26
	else if (g_ctrl && ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 1;
	buf[n++] = ch;
	write(master, buf, n);
}

int main(void) {
	int fbfd = open("/dev/fb0", O_RDWR);
	struct fb_var var; struct fb_fix fix;
	ioctl(fbfd, FBIOGET_VSCREENINFO, &var);
	ioctl(fbfd, FBIOGET_FSCREENINFO, &fix);
	g_fb = (uint8_t*) mmap(0, fix.smem_len, 3, 1, fbfd, 0);
	g_pitch = fix.line_length; g_xres = var.xres; g_yres = var.yres;
	g_cols = g_xres / CW; if (g_cols > MAXC) g_cols = MAXC;
	g_rows = g_yres / CH; if (g_rows > MAXR) g_rows = MAXR;
	g_top = 0; g_bot = g_rows - 1;
	clearRegion(0, 0, g_cols - 1, g_rows - 1);

	int master = open("/dev/ptmx", O_RDWR);
	int inp = open("/dev/input0", O_RDONLY);

	/* The shell runs raw (it does its own line editing/echo) but keep ISIG so Ctrl+C still
	 * interrupts the foreground program; no output post-processing (the VT handles \n). */
	struct termios t;
	tcgetattr(master, &t);
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_lflag |= ISIG;
	t.c_oflag = 0;
	t.c_iflag = ICRNL;
	tcsetattr(master, TCSANOW, &t);

	int pid = fork();
	if (pid == 0) {
		int s = open("/dev/pts0", O_RDWR);
		dup2(s, 0); dup2(s, 1); dup2(s, 2);
		if (s > 2) close(s);
		close(master); close(inp); close(fbfd);
		char* argv[] = { (char*) "nsh", 0 };
		char* envp[] = { 0 };
		execve("/disks/main/nanos/bin/nsh.nxe", argv, envp);
		_exit(127);
	}

	struct pollfd pf[2];
	pf[0].fd = master; pf[0].events = POLLIN;
	pf[1].fd = inp;    pf[1].events = POLLIN;
	for (;;) {
		poll(pf, 2, -1);
		if (pf[0].revents & POLLIN) {            // shell output -> render
			unsigned char ob[512];
			int n = read(master, ob, sizeof ob);
			for (int i = 0; i < n; i++)
				feed(ob[i]);
		}
		if (pf[1].revents & POLLIN) {            // key events -> master
			unsigned char ev[64];
			int n = read(inp, ev, sizeof ev);
			for (int i = 0; i + 1 < n; i += 2)
				keyEvent(master, ev[i], ev[i + 1]);
		}
	}
	return 0;
}
