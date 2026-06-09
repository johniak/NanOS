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
static int g_kbdFd = -1;           /* /dev/input0 (key down/up events) */

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

	/* Raw console mode just to silence echo (cooked mode would print typed keys onto the
	 * game screen). Real key events come from /dev/input0 (key down/up), not the console. */
	termmode(1);
	g_kbdFd = open("/dev/input0", O_RDONLY);
	/* Drain any scancodes queued while the shell was running. */
	if (g_kbdFd >= 0) {
		unsigned char tmp[64];
		while (read(g_kbdFd, tmp, sizeof tmp) > 0)
			;
	}
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
 * Real key events from /dev/input0: each is [code][down], code = normalised PS/2 set-1
 * scancode (bit7 = extended key), down = 1 press / 0 release. We map code -> Doom key and
 * forward the exact press/release, so movement (which reads the held key state across
 * tics) works and stops precisely on the break code — no timeout, no guessing. The
 * default Doom control scheme falls right out of the standard keys. */
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

/* PS/2 set-1 base scancode (0x00-0x7F) -> Doom key, for non-extended keys. 0 = ignore. */
static const unsigned char sc2k[128] = {
	[0x01] = KEY_ESCAPE,
	[0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
	[0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
	[0x0C] = KEY_MINUS, [0x0D] = KEY_EQUALS,
	[0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
	[0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
	[0x1C] = KEY_ENTER,
	[0x1D] = KEY_FIRE,                 /* Left Ctrl = fire (Doom default) */
	[0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
	[0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
	[0x2A] = KEY_RSHIFT,               /* Left Shift = run */
	[0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
	[0x31] = 'n', [0x32] = 'm',
	[0x36] = KEY_RSHIFT,               /* Right Shift = run */
	[0x38] = KEY_RALT,                 /* Left Alt = strafe */
	[0x39] = KEY_USE,                  /* Space = use (Doom default) */
	[0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4, [0x3F] = KEY_F5,
	[0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8, [0x43] = KEY_F9, [0x44] = KEY_F10,
};

/* Map a normalised scancode (from /dev/input0) to a Doom key, or 0 to ignore. */
static unsigned char scToDoom(unsigned char code) {
	if (code & 0x80) {                 /* extended (0xE0-prefixed) keys */
		switch (code & 0x7F) {
		case 0x48: return KEY_UPARROW;
		case 0x50: return KEY_DOWNARROW;
		case 0x4B: return KEY_LEFTARROW;
		case 0x4D: return KEY_RIGHTARROW;
		case 0x1D: return KEY_FIRE;    /* Right Ctrl */
		case 0x38: return KEY_RALT;    /* Right Alt = strafe */
		default:   return 0;
		}
	}
	return sc2k[code & 0x7F];
}

/* Read queued key events from /dev/input0 and turn them into Doom press/release events. */
static void pollInput() {
	if (g_kbdFd < 0)
		return;
	unsigned char b[64];
	int n = read(g_kbdFd, b, sizeof b);
	for (int i = 0; n > 0 && i + 1 < n; i += 2) {     /* [code][down] records */
		unsigned char key = scToDoom(b[i]);
		if (key)
			evPush(b[i + 1] ? 1 : 0, key);
	}
}

int DG_GetKey(int* pressed, unsigned char* key) {
	if (g_evHead == g_evTail)
		pollInput();
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
	char* dargv[] = { "doom", "-iwad", "/disks/main/apps/doom/doom1.wad", 0 };
	doomgeneric_Create(3, dargv);
	for (;;)
		doomgeneric_Tick();
	return 0;
}
