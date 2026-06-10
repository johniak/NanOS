/*
 * nwm.c — the NanWM compositor I/O shell. The ONLY part that touches hardware: it owns
 * /dev/fb0 (mmap), /dev/input0 (keyboard) and /dev/input1 (mouse), spawns GUI clients over
 * an inherited request/event pipe pair (fds 3/4), runs a poll() loop that COALESCES all
 * pending input/requests then composites + blits ONCE, and drains each client's event ring
 * to its pipe on POLLOUT. All policy lives in nwm_core/nw_compose (pure, host-tested).
 *
 * Launched like `startx` from the shell: it puts the console in raw mode (so the kernel
 * stops echo-drawing keystrokes onto the framebuffer), runs the desktop, and on the last
 * client exiting restores the console and returns.
 */
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#include "nwm_core.h"
#include "nw_compose.h"
#include "nwproto.h"
#include "nw_gfx.h"
#include "SyscallNr.h"           /* SYS_reboot for the Shutdown button */

/* Power the machine off via the kernel (privileged port I/O lives in the kernel). */
static void sys_poweroff(void) { __asm__ __volatile__("int $0x80" : : "a"(SYS_reboot) : "memory"); }

extern void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long off);
int ioctl(int fd, unsigned long request, ...);
int termmode(int raw);                       /* SYS_termmode: 1 = raw console, 0 = cooked  */
int waitpid(int pid, int *status, int opt);
#ifndef WNOHANG
#define WNOHANG 1
#endif

/* fbdev (mirrors user/term/nterm.c). */
struct fb_var { uint32_t xres, yres, xv, yv, xo, yo, bpp, gray; uint32_t pad[40]; };
struct fb_fix { char id[16]; uint32_t smem_start, smem_len, type, type_aux, visual;
	uint16_t xp, yp, yw; uint32_t line_length; uint32_t pad[8]; };
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

/* mouse evdev (mirrors kext/mouse/MouseDevice.h). */
struct input_event { uint32_t tv_sec, tv_usec; uint16_t type, code; int32_t value; };
enum { EV_SYN = 0, EV_KEY = 1, EV_REL = 2 };
enum { REL_X = 0, REL_Y = 1 };
enum { BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112 };

#define CLIENT_OUTCAP   (64 * 1024)
#define CLIENT_COMMITCAP (512 * 1024)   /* max COMMIT payload reassembled per client */
#define NWNOTE_PATH "/disks/main/apps/nwnote/nwnote.nxe"

/* framebuffer + the cached scene (desktop+windows, NO cursor) */
static uint8_t  *g_fb;
static uint32_t  g_pitch, g_xres, g_yres;
static uint32_t *g_scene;                 /* composed scene, blitted to fb by damage rect */
static struct nw_surface g_scene_surf;    /* wraps g_scene (stride = xres)                */
static struct nw_surface g_fb_surf;       /* wraps the LFB (stride = pitch/4)             */
static int g_prev_cx = -1, g_prev_cy = -1;/* last drawn cursor position                   */

/* per-client shell state (parallel to nw_server's client slots) */
static int             cl_req[NW_MAX_CLIENTS], cl_evt[NW_MAX_CLIENTS], cl_pid[NW_MAX_CLIENTS];
static struct nw_decoder cl_dec[NW_MAX_CLIENTS];
static unsigned char  *cl_pay[NW_MAX_CLIENTS];
static unsigned char  *cl_out[NW_MAX_CLIENTS];

/* per-window-slot backing buffer the shell allocated (freed when the slot frees) */
static uint32_t *g_winbuf[NW_MAX_WINDOWS];

static struct nw_server S;

static void set_cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }
static void set_nonblock(int fd) { fcntl(fd, F_SETFL, O_NONBLOCK); }

/* Spawn a GUI client with an inherited request(3)/event(4) pipe pair into nw_server slot. */
static int spawn_client(int slot, const char *path)
{
	int reqp[2], evtp[2];
	if (pipe(reqp) < 0 || pipe(evtp) < 0)
		return -1;
	set_cloexec(reqp[0]); set_cloexec(reqp[1]);
	set_cloexec(evtp[0]); set_cloexec(evtp[1]);

	int pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		dup2(reqp[1], 3);                 /* client writes requests on fd 3 */
		dup2(evtp[0], 4);                 /* client reads events on fd 4    */
		fcntl(3, F_SETFD, 0);             /* keep 3/4 across execve         */
		fcntl(4, F_SETFD, 0);
		char *argv[] = { (char *) "nwnote", 0 };
		char *envp[] = { (char *) "NW_DISPLAY=1", 0 };
		execve(path, argv, envp);
		_exit(127);
	}
	cl_req[slot] = reqp[0];               /* parent reads requests   */
	cl_evt[slot] = evtp[1];               /* parent writes events    */
	cl_pid[slot] = pid;
	close(reqp[1]); close(evtp[0]);
	set_nonblock(cl_req[slot]);
	set_nonblock(cl_evt[slot]);

	cl_pay[slot] = (unsigned char *) malloc(CLIENT_COMMITCAP);
	cl_out[slot] = (unsigned char *) malloc(CLIENT_OUTCAP);
	if (!cl_pay[slot] || !cl_out[slot])
		return -1;
	nw_decoder_init(&cl_dec[slot], cl_pay[slot], CLIENT_COMMITCAP);
	nw_client_connect(&S, slot, cl_out[slot], CLIENT_OUTCAP);
	return 0;
}

/* Resolve a Run command to a launchable path, the way the shell looks up apps. */
static int file_exists(const char *p) { int fd = open(p, 0); if (fd >= 0) { close(fd); return 1; } return 0; }
static int resolve_cmd(const char *cmd, char *out, int cap)
{
	if (cmd[0] == '/') {
		if (!file_exists(cmd)) return 0;
		int i = 0; for (; cmd[i] && i < cap - 1; i++) out[i] = cmd[i]; out[i] = 0; return 1;
	}
	snprintf(out, cap, "/disks/main/apps/%s/%s.nxe", cmd, cmd); if (file_exists(out)) return 1;
	snprintf(out, cap, "/disks/main/bin/%s.nxe", cmd);          if (file_exists(out)) return 1;
	snprintf(out, cap, "/disks/main/nanos/bin/%s.nxe", cmd);    if (file_exists(out)) return 1;
	return 0;
}
static int free_slot(void)
{
	for (int i = 0; i < NW_MAX_CLIENTS; i++)
		if (cl_req[i] < 0) return i;
	return -1;
}

static void disconnect(int slot)
{
	nw_client_disconnect(&S, slot);
	close(cl_req[slot]); close(cl_evt[slot]);
	cl_req[slot] = cl_evt[slot] = -1;
	if (cl_pid[slot] > 0) { int st; waitpid(cl_pid[slot], &st, WNOHANG); cl_pid[slot] = 0; }
	free(cl_pay[slot]); free(cl_out[slot]);
	cl_pay[slot] = cl_out[slot] = 0;
}

/* read all queued keyboard events ([code][down], non-blocking) and feed the core */
static void drain_keyboard(int fd)
{
	unsigned char b[128];
	int n;
	while ((n = (int) read(fd, b, sizeof b)) > 0)
		for (int i = 0; i + 1 < n; i += 2)
			nw_key(&S, b[i], b[i + 1]);
}

/* read all queued mouse input_events, fold REL/BTN, fire a pointer update per SYN_REPORT */
static void drain_mouse(int fd)
{
	static int cx = -1, cy = -1, btn = 0;
	if (cx < 0) { cx = (int) g_xres / 2; cy = (int) g_yres / 2; }
	struct input_event ev[32];
	int n;
	while ((n = (int) read(fd, ev, sizeof ev)) > 0) {
		int cnt = n / (int) sizeof(struct input_event);
		for (int i = 0; i < cnt; i++) {
			struct input_event *e = &ev[i];
			if (e->type == EV_REL) {
				if (e->code == REL_X) cx += e->value;
				else if (e->code == REL_Y) cy += e->value;
			} else if (e->type == EV_KEY) {
				int m = e->code == BTN_LEFT ? NW_BTN_LEFT :
				        e->code == BTN_RIGHT ? NW_BTN_RIGHT :
				        e->code == BTN_MIDDLE ? NW_BTN_MIDDLE : 0;
				if (m) { if (e->value) btn |= m; else btn &= ~m; }
			} else if (e->type == EV_SYN) {
				if (cx < 0) cx = 0;
				if (cy < 0) cy = 0;
				if (cx >= (int) g_xres) cx = (int) g_xres - 1;
				if (cy >= (int) g_yres) cy = (int) g_yres - 1;
				nw_pointer(&S, cx, cy, btn);
			}
		}
	}
}

/* read all queued bytes from a client's request pipe; returns 0 on EOF (client gone) */
static int drain_client(int slot)
{
	unsigned char b[4096];
	int n;
	while ((n = (int) read(cl_req[slot], b, sizeof b)) > 0) {
		const unsigned char *p = b, *end = b + n;
		while (nw_decoder_next(&cl_dec[slot], &p, end))
			nw_client_msg(&S, slot, &cl_dec[slot].msg, cl_dec[slot].payload);
	}
	if (n == 0)
		return 0;                          /* EOF: client closed its end */
	return 1;
}

/* bind freshly created windows to a pixel buffer; free buffers of destroyed windows */
static void reconcile_buffers(void)
{
	struct nw_window *w;
	while ((w = nw_window_needs_buffer(&S)) != 0) {
		int slot = (int) (w - S.win);
		g_winbuf[slot] = (uint32_t *) malloc((size_t) w->cw * w->ch * 4);
		if (g_winbuf[slot]) memset(g_winbuf[slot], 0, (size_t) w->cw * w->ch * 4);
		w->buf = g_winbuf[slot];
	}
	for (int i = 0; i < NW_MAX_WINDOWS; i++)
		if (!S.win[i].used && g_winbuf[i]) { free(g_winbuf[i]); g_winbuf[i] = 0; }
}

/* try to push a client's queued events to its pipe (non-blocking) */
static void flush_output(int slot)
{
	for (;;) {
		uint32_t len = 0;
		const unsigned char *p = nw_outq_peek(&S, slot, &len);
		if (!len)
			break;
		int n = (int) write(cl_evt[slot], p, len);
		if (n <= 0)
			break;                          /* would block / error: try again next poll */
		nw_outq_ack(&S, slot, (uint32_t) n);
	}
}

/* Copy a rectangle of the cached scene to the framebuffer (honouring fb pitch). */
static void blit_scene(int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int) g_xres) w = (int) g_xres - x;
	if (y + h > (int) g_yres) h = (int) g_yres - y;
	if (w <= 0 || h <= 0)
		return;
	for (int r = 0; r < h; r++)
		memcpy(g_fb + (size_t) (y + r) * g_pitch + (size_t) x * 4,
		       g_scene + (size_t) (y + r) * g_xres + x, (size_t) w * 4);
}

/* Present a frame: erase the old cursor, blit only the damaged scene region (if the scene
 * changed), then draw the cursor as an overlay on the framebuffer. Plain mouse motion costs
 * a few tiny blits — never a full-screen repaint. */
static void present(void)
{
	if (!S.dirty && S.cursor_x == g_prev_cx && S.cursor_y == g_prev_cy)
		return;                                  /* nothing changed */
	if (g_prev_cx >= 0)
		blit_scene(g_prev_cx, g_prev_cy, NW_CURSOR_W, NW_CURSOR_H);   /* erase old cursor */
	if (S.dirty) {
		int dx, dy, dw, dh;
		int have = nw_peek_damage(&S, &dx, &dy, &dw, &dh);
		if (have) nw_surface_clip(&g_scene_surf, dx, dy, dw, dh);  /* recompose only the damage */
		else      nw_surface_noclip(&g_scene_surf);
		nw_compose_scene(&S, &g_scene_surf);     /* clipped: cost ∝ damage, not whole screen */
		nw_surface_noclip(&g_scene_surf);
		nw_take_damage(&S, &dx, &dy, &dw, &dh);  /* consume it */
		if (have)
			blit_scene(dx, dy, dw, dh);          /* only the changed region */
		else
			blit_scene(0, 0, (int) g_xres, (int) g_yres);   /* first frame / fallback */
		S.dirty = 0;
	}
	blit_scene(S.cursor_x, S.cursor_y, NW_CURSOR_W, NW_CURSOR_H);     /* scene under cursor */
	nw_draw_cursor(&g_fb_surf, S.cursor_x, S.cursor_y);
	g_prev_cx = S.cursor_x; g_prev_cy = S.cursor_y;
}

int main(void)
{
	int fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0) { printf("nwm: no /dev/fb0\n"); return 1; }
	struct fb_var var; struct fb_fix fix;
	ioctl(fbfd, FBIOGET_VSCREENINFO, &var);
	ioctl(fbfd, FBIOGET_FSCREENINFO, &fix);
	g_xres = var.xres; g_yres = var.yres; g_pitch = fix.line_length;
	g_fb = (uint8_t *) mmap(0, fix.smem_len, 3, 1, fbfd, 0);
	if (g_fb == (uint8_t *) -1 || !g_fb) { printf("nwm: fb mmap failed\n"); return 1; }
	g_scene = (uint32_t *) malloc((size_t) g_xres * g_yres * 4);
	if (!g_scene) { printf("nwm: no memory for scene buffer\n"); return 1; }
	g_scene_surf.px = g_scene; g_scene_surf.w = (int) g_xres; g_scene_surf.h = (int) g_yres;
	g_scene_surf.stride = (int) g_xres;
	g_fb_surf.px = (uint32_t *) g_fb; g_fb_surf.w = (int) g_xres; g_fb_surf.h = (int) g_yres;
	g_fb_surf.stride = (int) (g_pitch / 4);

	int in0 = open("/dev/input0", O_RDONLY | O_NONBLOCK);
	int in1 = open("/dev/input1", O_RDONLY | O_NONBLOCK);
	set_cloexec(fbfd); set_cloexec(in0); set_cloexec(in1);

	signal(SIGPIPE, SIG_IGN);                 /* a client exiting must not kill the server */

	for (int i = 0; i < NW_MAX_CLIENTS; i++) { cl_req[i] = cl_evt[i] = -1; }
	nw_server_init(&S, (int) g_xres, (int) g_yres);

	termmode(1);                              /* silence the kernel console echo-draw */

	spawn_client(0, NWNOTE_PATH);
	spawn_client(1, NWNOTE_PATH);

	S.dirty = 1;
	present();                                /* first frame: desktop + cursor */

	for (;;) {
		struct pollfd pfd[2 + NW_MAX_CLIENTS * 2];
		int n = 0;
		pfd[n].fd = in0; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
		pfd[n].fd = in1; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
		for (int i = 0; i < NW_MAX_CLIENTS; i++) {
			if (cl_req[i] < 0)
				continue;
			pfd[n].fd = cl_req[i]; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
			if (nw_outq_pending(&S, i) > 0) {
				pfd[n].fd = cl_evt[i]; pfd[n].events = POLLOUT; pfd[n].revents = 0; n++;
			}
		}
		/* The desktop persists with zero windows — leave only via Quit/Shutdown. */
		poll(pfd, n, -1);

		/* COALESCE: drain every input + request before drawing */
		drain_keyboard(in0);
		drain_mouse(in1);
		for (int i = 0; i < NW_MAX_CLIENTS; i++)
			if (cl_req[i] >= 0 && drain_client(i) == 0)
				disconnect(i);

		reconcile_buffers();

		if (S.want_shutdown) {                 /* Shutdown button: power the machine off */
			termmode(0);
			memset(g_fb, 0, (size_t) g_pitch * g_yres);
			sys_poweroff();                    /* does not return */
		}
		if (S.want_quit)                       /* Quit button: leave the desktop */
			break;

		char cmd[NW_RUN_MAX];                  /* Run dialog (Super+R): launch the typed app */
		if (nw_run_take_spawn(&S, cmd, sizeof cmd)) {
			char path[256];
			int slot = free_slot();
			if (slot >= 0 && resolve_cmd(cmd, path, sizeof path))
				spawn_client(slot, path);
		}

		present();   /* recomposes only on scene damage; always cheap cursor overlay */

		/* flush queued events; drop clients whose ring overflowed */
		for (int i = 0; i < NW_MAX_CLIENTS; i++) {
			if (cl_req[i] < 0)
				continue;
			flush_output(i);
			if (nw_client_is_dead(&S, i))
				disconnect(i);
		}
	}

	termmode(0);
	memset(g_fb, 0, (size_t) g_pitch * g_yres);   /* clear the desktop on the way out */
	printf("nwm: exit\n");
	return 0;
}
