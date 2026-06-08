/*
 * doomgeneric_nanos.c — the NanOS platform layer for doomgeneric (Doom).
 *
 * Implements the DG_* hooks on top of NanOS's Unix-shaped facilities, all built in the
 * earlier stages of the port:
 *   - display:  /dev/fb0 (open + fbdev ioctls + mmap), 640x400 letterboxed in 1024x768
 *   - timing:   clock_gettime(CLOCK_MONOTONIC) + nanosleep
 *   - input:    raw, non-blocking console reads (fcntl O_NONBLOCK), arrows as ESC[ codes
 *   - storage:  the WAD on the ext disk (read-only) + config/savegames on the /tmp tmpfs
 *
 * No swizzle: DG_ScreenBuffer and the framebuffer are both XRGB8888 (0x00RRGGBB).
 */
#include "doomgeneric.h"
#include "doomkeys.h"
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stdint.h>

int termmode(int raw);   /* NanOS console mode: 0 cooked, 1 raw (libc-glue) */

/* ---- framebuffer (subset of <linux/fb.h>, matches drivers/Fbdev.h) ---------------- */
extern int ioctl(int fd, unsigned long request, ...);
extern void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long off);

struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_fix_screeninfo {
	char id[16];
	uint32_t smem_start, smem_len, type, type_aux, visual;
	uint16_t xpanstep, ypanstep, ywrapstep;
	uint32_t line_length, mmio_start, mmio_len, accel;
	uint16_t capabilities, reserved[2];
};
struct fb_var_screeninfo {
	uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
	uint32_t bits_per_pixel, grayscale;
	struct fb_bitfield red, green, blue, transp;
	uint32_t nonstd, activate, height, width, accel_flags;
	uint32_t pixclock, lm, rm, um, lmar, hslen, vslen, sync, vmode, rotate, cs, resv[4];
};
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

static unsigned char* g_fb;        /* mmap'd linear framebuffer */
static uint32_t g_pitch;           /* bytes per scanline */
static uint32_t g_fbw, g_fbh;      /* screen resolution */
static uint32_t g_xoff, g_yoff;    /* top-left of the centered 640x400 image */

void DG_Init() {
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0)
		return;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	ioctl(fd, FBIOGET_VSCREENINFO, &var);
	ioctl(fd, FBIOGET_FSCREENINFO, &fix);
	g_pitch = fix.line_length;
	g_fbw = var.xres;
	g_fbh = var.yres;
	g_fb = (unsigned char*) mmap(0, fix.smem_len, 3 /*RW*/, 1 /*SHARED*/, fd, 0);
	if (g_fb == (void*) -1)
		g_fb = 0;
	/* Center the 640x400 frame in the screen (letterbox, no scaling). */
	g_xoff = g_fbw > DOOMGENERIC_RESX ? (g_fbw - DOOMGENERIC_RESX) / 2 : 0;
	g_yoff = g_fbh > DOOMGENERIC_RESY ? (g_fbh - DOOMGENERIC_RESY) / 2 : 0;

	/* Non-blocking raw keyboard, so DG_GetKey can poll each frame without stalling. */
	termmode(1);
	fcntl(0, F_SETFL, O_NONBLOCK);
}

void DG_DrawFrame() {
	if (!g_fb)
		return;
	/* On the very first frame, black out the whole screen once — this clears the boot +
	 * Doom startup dmesg that printed to the framebuffer console before the game began,
	 * leaving Doom's centered image on a clean black letterbox. */
	static int cleared = 0;
	if (!cleared) {
		memset(g_fb, 0, g_pitch * g_fbh);
		cleared = 1;
	}
	/* One memcpy per scanline: DG_ScreenBuffer rows are 640 XRGB pixels; blit each to the
	 * centered position, striding the screen by its pitch. 400 row copies/frame. */
	for (uint32_t y = 0; y < DOOMGENERIC_RESY; y++) {
		unsigned char* dst = g_fb + (g_yoff + y) * g_pitch + g_xoff * 4;
		const unsigned char* src = (const unsigned char*) (DG_ScreenBuffer + y * DOOMGENERIC_RESX);
		memcpy(dst, src, DOOMGENERIC_RESX * 4);
	}
}

uint32_t DG_GetTicksMs() {
	struct timespec ts = { 0, 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) ((uint32_t) ts.tv_sec * 1000u + (uint32_t) (ts.tv_nsec / 1000000));
}

void DG_SleepMs(uint32_t ms) {
	struct timespec req;
	req.tv_sec = ms / 1000;
	req.tv_nsec = (long) (ms % 1000) * 1000000L;
	nanosleep(&req, 0);
}

/* ---- input -------------------------------------------------------------------------
 * The tty gives key-down only (no key-up), so we synthesize a release immediately after
 * each press (a "tap"): holding a key autorepeats, which reads as repeated taps — enough
 * for movement, fire, use, menus and weapon select. Events are queued and handed out one
 * per DG_GetKey call (doomgeneric polls until it returns 0). */
#define EVQ 256
static struct { int pressed; unsigned char key; } g_evq[EVQ];
static int g_evHead, g_evTail;

static void evPush(int pressed, unsigned char key) {
	int n = (g_evHead + 1) % EVQ;
	if (n != g_evTail) {
		g_evq[g_evHead].pressed = pressed;
		g_evq[g_evHead].key = key;
		g_evHead = n;
	}
}

/* Map a console byte to a Doom key code. Letters are lowercased; digits/letters pass
 * through (Doom reads them directly for weapons and y/n prompts). */
static unsigned char toDoomKey(int c) {
	switch (c) {
	case '\n': case '\r': return KEY_ENTER;
	case 27:              return KEY_ESCAPE;   /* a bare ESC (not an arrow sequence) */
	case 8: case 0x7f:    return KEY_BACKSPACE;
	case '\t':            return KEY_TAB;
	case ' ':             return KEY_FIRE;     /* space shoots */
	case 'e': case 'E':   return KEY_USE;      /* e opens doors / flips switches */
	default:
		if (c >= 'A' && c <= 'Z') return (unsigned char) (c - 'A' + 'a');
		return (unsigned char) c;
	}
}

/* Drain all currently-buffered console bytes into the event queue (press+release each). */
static void drainInput() {
	unsigned char b[64];
	int n = read(0, b, sizeof b);
	if (n <= 0)
		return;
	for (int i = 0; i < n; i++) {
		unsigned char k;
		if (b[i] == 27 && i + 2 < n && b[i + 1] == '[') {
			switch (b[i + 2]) {                /* arrow keys: ESC [ A/B/C/D */
			case 'A': k = KEY_UPARROW; break;
			case 'B': k = KEY_DOWNARROW; break;
			case 'C': k = KEY_RIGHTARROW; break;
			case 'D': k = KEY_LEFTARROW; break;
			default:  k = KEY_ESCAPE; break;
			}
			i += 2;
		} else {
			k = toDoomKey(b[i]);
		}
		evPush(1, k);   /* press ... */
		evPush(0, k);   /* ... then release (tap) */
	}
}

int DG_GetKey(int* pressed, unsigned char* key) {
	if (g_evHead == g_evTail)
		drainInput();
	if (g_evHead == g_evTail)
		return 0;
	*pressed = g_evq[g_evTail].pressed;
	*key = g_evq[g_evTail].key;
	g_evTail = (g_evTail + 1) % EVQ;
	return 1;
}

void DG_SetWindowTitle(const char* title) {
	(void) title;   /* no windows */
}

int main(int argc, char** argv) {
	(void) argc; (void) argv;
	/* Relative file writes (default.cfg, savegames) land on the writable tmpfs. */
	chdir("/tmp");
	/* Point Doom at the shareware WAD on the read-only disk (the built-in IWAD search
	 * uses paths we don't have, so pass it explicitly). */
	char* dargv[] = { "doom", "-iwad", "/disks/main/nanos/doom1.wad", 0 };
	doomgeneric_Create(3, dargv);
	for (;;)
		doomgeneric_Tick();
	return 0;
}
