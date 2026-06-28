/*
 * nterm — NanOS framebuffer terminal emulator (the "fbterm/st" of NanOS).
 *
 * Owns the framebuffer and is the VT for a shell running on a pty:
 *   - opens /dev/ptmx (master); forks a child that puts /dev/pts0 on stdin/out/err and
 *     execs the shell (the slave is its controlling terminal);
 *   - event loop: poll(master, /dev/input0); keystrokes -> bytes -> write(master);
 *     shell output (master) -> the shared VT engine (vt.c) -> rasterized to /dev/fb0.
 *
 * The escape-sequence parsing + screen grid live in vt.c (shared with the windowed terminal
 * terminal); this file is the framebuffer + keyboard + pty glue, repainting only dirty rows.
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <pwd.h>
#include <string.h>
#include <sys/termios.h>
#include "vt.h"

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

static vt T;

static void cellDraw(int cx, int cy) {
	if (cx < 0 || cy < 0 || cx >= T.cols || cy >= T.rows) return;
	vt_cell* c = &T.grid[cy][cx];
	uint32_t fgc = vt_pal(c->fg), bgc = vt_pal(c->bg);
	const unsigned char* gl = nx_font8x16[c->ch];
	for (int y = 0; y < CH; y++) {
		uint32_t* row = (uint32_t*) (g_fb + (cy * CH + y) * g_pitch) + cx * CW;
		unsigned char bits = gl[y];
		for (int x = 0; x < CW; x++)
			row[x] = (bits & (0x80 >> x)) ? fgc : bgc;
	}
}

static void renderDirty(void) {
	for (int y = 0; y < T.rows; y++)
		if (T.dirty[y]) { for (int x = 0; x < T.cols; x++) cellDraw(x, y); T.dirty[y] = 0; }
}

/* ---- keyboard (/dev/input0 evdev) -> bytes to the master ---- */
static int g_shift, g_ctrl;
static const char kmap[128] = {
	0,0,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
	'q','w','e','r','t','y','u','i','o','p','[',']','\r',0,'a','s',
	'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
	'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0 };
static const char kmapsh[128] = {
	0,0,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
	'Q','W','E','R','T','Y','U','I','O','P','{','}','\r',0,'A','S',
	'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
	'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0 };

static void keyEvent(int master, unsigned char code, unsigned char down) {
	int ext = code & 0x80;
	int sc = code & 0x7f;
	if (!ext && (sc == 0x2a || sc == 0x36)) { g_shift = down; return; }
	if (sc == 0x1d) { g_ctrl = down; return; }
	if (!down) return;
	if (ext) {
		const char* seq = 0;
		if (sc == 0x48) seq = "\x1b[A"; else if (sc == 0x50) seq = "\x1b[B";
		else if (sc == 0x4d) seq = "\x1b[C"; else if (sc == 0x4b) seq = "\x1b[D";
		else if (sc == 0x47) seq = "\x1b[H"; else if (sc == 0x4f) seq = "\x1b[F";
		else if (sc == 0x49) seq = "\x1b[5~"; else if (sc == 0x51) seq = "\x1b[6~";
		else if (sc == 0x52) seq = "\x1b[2~"; else if (sc == 0x53) seq = "\x1b[3~";
		if (seq) { int l = 0; while (seq[l]) l++; write(master, seq, l); }
		return;
	}
	{
		const char* fk = 0;
		switch (sc) {
		case 0x3b: fk = "\x1bOP"; break;  case 0x3c: fk = "\x1bOQ"; break;
		case 0x3d: fk = "\x1bOR"; break;  case 0x3e: fk = "\x1bOS"; break;
		case 0x3f: fk = "\x1b[15~"; break; case 0x40: fk = "\x1b[17~"; break;
		case 0x41: fk = "\x1b[18~"; break; case 0x42: fk = "\x1b[19~"; break;
		case 0x43: fk = "\x1b[20~"; break; case 0x44: fk = "\x1b[21~"; break;
		case 0x57: fk = "\x1b[23~"; break; case 0x58: fk = "\x1b[24~"; break;
		}
		if (fk) { int l = 0; while (fk[l]) l++; write(master, fk, l); return; }
	}
	char ch = g_shift ? kmapsh[sc] : kmap[sc];
	if (!ch) return;
	if (g_ctrl && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;
	else if (g_ctrl && ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 1;
	write(master, &ch, 1);
}

int main(void) {
	int fbfd = open("/dev/fb0", O_RDWR);
	struct fb_var var; struct fb_fix fix;
	ioctl(fbfd, FBIOGET_VSCREENINFO, &var);
	ioctl(fbfd, FBIOGET_FSCREENINFO, &fix);
	g_fb = (uint8_t*) mmap(0, fix.smem_len, 3, 1, fbfd, 0);
	g_pitch = fix.line_length; g_xres = var.xres; g_yres = var.yres;
	vt_init(&T, g_xres / CW, g_yres / CH);
	renderDirty();

	int master = open("/dev/ptmx", O_RDWR);
	int inp = open("/dev/input0", O_RDONLY);

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
		struct passwd* pw = getpwuid(getuid());
		const char* shell = (pw && pw->pw_shell && pw->pw_shell[0])
		                  ? pw->pw_shell : "/disks/main/nanos/bin/nsh.nxe";
		const char* base = strrchr(shell, '/');
		base = base ? base + 1 : shell;
		static char name0[64];
		{ int i = 0; while (base[i] && i < (int) sizeof name0 - 1) { name0[i] = base[i]; i++; }
		  name0[i] = 0; if (i >= 4 && strcmp(name0 + i - 4, ".nxe") == 0) name0[i - 4] = 0; }
		static char shellvar[160]; strcpy(shellvar, "SHELL="); strncat(shellvar, shell, sizeof shellvar - 7);
		static char homevar[160]; strcpy(homevar, "HOME=");
		strncat(homevar, (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : "/", sizeof homevar - 6);
		char* argv[] = { name0, 0 };
		char* envp[] = { (char*) "TERM=xterm-256color",
		                 (char*) "TERMINFO=/disks/main/nanos/share/terminfo",
		                 (char*) "PATH=/disks/main/nanos/bin:/disks/main/bin", shellvar, homevar, 0 };
		execve(shell, argv, envp);
		char* fbargv[] = { (char*) "nsh", 0 };
		execve("/disks/main/nanos/bin/nsh.nxe", fbargv, envp);
		_exit(127);
	}

	struct pollfd pf[2];
	pf[0].fd = master; pf[0].events = POLLIN;
	pf[1].fd = inp;    pf[1].events = POLLIN;
	for (;;) {
		poll(pf, 2, -1);
		if (pf[0].revents & POLLIN) {
			unsigned char ob[512];
			int n = read(master, ob, sizeof ob);
			if (n > 0) { vt_feed(&T, ob, n); renderDirty(); }
		}
		if (pf[1].revents & POLLIN) {
			unsigned char ev[64];
			int n = read(inp, ev, sizeof ev);
			for (int i = 0; i + 1 < n; i += 2) keyEvent(master, ev[i], ev[i + 1]);
		}
	}
	return 0;
}
