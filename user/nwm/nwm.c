/*
 * nwm.c — the nanowm compositor I/O shell. The ONLY part that touches hardware: it owns
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
#include <time.h>

#include "nwm_core.h"
#include "nw_compose.h"
#include "nwproto.h"
#include "nw_gfx.h"
#include "nwui_png.h"             /* decode the branded wallpaper.png at runtime (toolkit decoder) */
#include "nw_settings.h"          /* desktop preferences (blur/transparency) from settings.yaml */
#include "nw_settings_path.h"     /* per-user prefs file in $HOME (writable by the desktop user) */
#ifdef NWM_GL
#include "nw_compose_gl.h"        /* Task 10: GL ES present backend (Mesa-linked build only) */
#include "glkms_init.h"           /* glkms_diag(): lossless startup markers for the Dell GL freeze */
#endif
#include "SyscallNr.h"           /* SYS_reboot for the Shutdown button */
#include "open/nwspawn.h"         /* the AF_UNIX launch socket `open` connects to */
#include <sys/socket.h>
#include <sys/un.h>

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
enum { REL_X = 0, REL_Y = 1, REL_WHEEL = 8 };
enum { BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112 };

#define CLIENT_OUTCAP   (64 * 1024)
#define CLIENT_COMMITCAP (512 * 1024)   /* max COMMIT payload reassembled per client */
#define NWNOTE_PATH "/disks/main/apps/notepad/notepad.nxe"
#define NWEXP_PATH  "/disks/main/apps/rsexp/rsexp.nxe"   /* the Files app (Rust icon explorer) */
#define NWSET_PATH  "/disks/main/apps/settings/settings.nxe"
#define NWTERM_PATH "/disks/main/apps/terminal/terminal.nxe"

/* framebuffer + the cached scene (desktop+windows, NO cursor) */
static uint8_t  *g_fb;
static uint32_t  g_pitch, g_xres, g_yres;
static uint32_t *g_scene;                 /* composed scene, blitted to fb by damage rect */
static uint32_t *g_scratch;               /* per-window opaque render buffer (then composited) */
static uint32_t *g_wall;                  /* pre-rendered gradient wallpaper                  */
static struct nw_surface g_scene_surf;    /* wraps g_scene (stride = xres)                */
static struct nw_surface g_scratch_surf;
static struct nw_surface g_wall_surf;
static struct nw_surface g_fb_surf;       /* wraps the LFB (stride = pitch/4)             */
static int g_prev_cx = -1, g_prev_cy = -1;/* last drawn cursor position                   */
static volatile int g_own = 0;            /* do we own the framebuffer? (VT_SETMODE, 0 until acquire) */
static int g_ttyfd = -1;                  /* our graphics VT (tty7) for KD/VT ioctls + VT_RELDISP    */
static int g_wakefd[2] = { -1, -1 };      /* self-pipe so VT signals wake the poll() loop            */
static int g_spawnfd = -1;                /* AF_UNIX listen socket: `open` requests app launches     */
static int g_started = 0;                 /* have we spawned the desktop yet? (once, on first own)  */
static volatile int g_force_full = 0;     /* next present() must repaint the WHOLE screen (after acquire) */
#ifdef NWM_GL
static int g_gl = 0;                       /* GL present backend live (Task 10); 0 => CPU fb0 path */
static int g_gl_fell_back = 0;             /* GL died mid-session: first CPU frame prints a liveness marker */
#endif
static uint32_t *g_bd;                     /* screen-aligned blurred-backdrop scratch     */
static uint32_t *g_bdlo;                   /* downsample scratch ((xres/F)*(yres/F) px)   */
static struct nw_surface g_bd_surf;
static struct nw_backdrop_ctx g_bdc;
static struct nw_settings g_set;           /* live desktop preferences (settings.yaml)    */
static int g_blur_on;                      /* derived: backdrop blur currently enabled    */

/* Overlay any settings found in the file at `path` onto g_set (no-op if it is absent/empty). */
static void overlay_settings_file(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return;
	char buf[1024];
	int n = (int) read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n > 0) nw_settings_parse(buf, n, &g_set);
}

/* (Re)load the desktop preferences and fold them into the compositor's runtime knobs. Layered:
 * shipped defaults, then the system-wide file (if an admin placed one), then the per-user file in
 * $HOME (which the unprivileged desktop can actually write — see nw_settings_path.h). The user
 * file wins. Missing/garbled files just leave earlier values. Caller marks the scene dirty. */
static void apply_settings(void)
{
	char up[256];
	nw_settings_defaults(&g_set);
	overlay_settings_file(NW_SETTINGS_PATH);            /* system-wide default (optional) */
	nw_settings_user_path(up, sizeof up);
	overlay_settings_file(up);                          /* per-user override (wins) */
	g_blur_on = g_set.blur && g_set.transparency;   /* blur only shows through translucent glass */
	g_bdc.win_alpha  = nw_settings_win_alpha(&g_set);
	g_bdc.dark_alpha = nw_settings_dark_alpha(&g_set);
	int r = nw_settings_blur_radius(&g_set);
	g_bdc.radius = r > 0 ? r : NW_BD_BLUR_RADIUS;
	nw_compose_set_theme(g_set.accent, g_set.corner_radius, g_set.shadow);
}

/* per-client shell state (parallel to nw_server's client slots) */
static int             cl_req[NW_MAX_CLIENTS], cl_evt[NW_MAX_CLIENTS], cl_pid[NW_MAX_CLIENTS];
static struct nw_decoder cl_dec[NW_MAX_CLIENTS];
static unsigned char  *cl_pay[NW_MAX_CLIENTS];
static unsigned char  *cl_out[NW_MAX_CLIENTS];

/* per-window-slot backing buffers the shell allocated (freed when the slot frees):
 * g_winbuf = client content (cw*ch); g_winframe = the cached chrome+content frame (fw*fh). */
static uint32_t *g_winbuf[NW_MAX_WINDOWS];
static uint32_t *g_winframe[NW_MAX_WINDOWS];
static uint32_t *g_winbackdrop[NW_MAX_WINDOWS];   /* per-window lo-res blurred-backdrop cache */

/* --- Frame-time profiling (Phase 0). Off by default; build with -DNWM_PROFILE=1 to print the
 * cost of each recompose+blit (the per-frame cost paid while dragging) to stderr — the kernel
 * text console behind the desktop — averaged over ~2 s windows. Integer microseconds only, so it
 * needs no float printf. Add nothing to the hot path when disabled. */
#ifndef NWM_PROFILE
#define NWM_PROFILE 0
#endif
#if NWM_PROFILE
static long g_pf_n, g_pf_sum, g_pf_min, g_pf_max, g_pf_window_ns;
static long pf_now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long) t.tv_sec * 1000000000L + t.tv_nsec;
}
static void pf_record(long dt_ns)
{
	long us = dt_ns / 1000;
	if (g_pf_n == 0 || us < g_pf_min) g_pf_min = us;
	if (g_pf_n == 0 || us > g_pf_max) g_pf_max = us;
	g_pf_sum += us; g_pf_n++;
	long t = pf_now_ns();
	if (g_pf_window_ns == 0) g_pf_window_ns = t;
	if (t - g_pf_window_ns >= 2000000000L) {
		long avg = g_pf_sum / g_pf_n;
		fprintf(stderr, "nwm: recompose n=%ld avg=%ldus min=%ldus max=%ldus (~%ld fps)\n",
		        g_pf_n, avg, g_pf_min, g_pf_max, avg ? 1000000L / avg : 0);
		g_pf_n = g_pf_sum = 0; g_pf_window_ns = t;
	}
}
#endif

static struct nw_server S;

static void set_cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }
static void set_nonblock(int fd) { fcntl(fd, F_SETFL, O_NONBLOCK); }

/* Spawn a GUI client with an inherited request(3)/event(4) pipe pair into nw_server slot. If
 * `password` is non-empty, launch ELEVATED: exec the setuid-root nanosu helper, which verifies the
 * password (root's) and runs the app as root ("authenticate to open"). */
extern char **environ;   /* nwm's login environment (HOME/USER/PATH/... from the greeter) */

/* Build a child's environment: inherit nwm's own env (so HOME/USER/PATH propagate — children need
 * HOME to resolve per-user files like settings and the open-with store) and append NW_DISPLAY=1
 * plus, for an elevated launch, NW_AUTH_PASS. Returns a NULL-terminated array (static; built in the
 * forked child just before execve, so the single-use static buffer is safe). */
static char **build_child_env(const char *passenv)
{
	static char *env[64];
	int n = 0;
	for (char **e = environ; e && *e && n < 61; e++)
		env[n++] = *e;
	env[n++] = (char *) "NW_DISPLAY=1";
	if (passenv && passenv[0]) env[n++] = (char *) passenv;
	env[n] = 0;
	return env;
}

static int spawn_client_priv(int slot, const char *path, const char *arg, const char *password)
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
		const char *base = path;                     /* argv[0] = the binary's basename */
		for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
		if (password && password[0]) {               /* elevated launch via setuid-root nanosu */
			static char passenv[272];
			snprintf(passenv, sizeof passenv, "NW_AUTH_PASS=%s", password);
			char **eenv = build_child_env(passenv);
			char *eav[4];
			eav[0] = (char *) "nanosu"; eav[1] = (char *) path;
			if (arg && arg[0]) { eav[2] = (char *) arg; eav[3] = 0; } else eav[2] = 0;
			execve("/disks/main/nanos/bin/nanosu.nxe", eav, eenv);
			_exit(127);
		}
		char *argv[3];
		argv[0] = (char *) base;
		if (arg && arg[0]) { argv[1] = (char *) arg; argv[2] = 0; }  /* open-with: argv[1] = a file */
		else argv[1] = 0;
		execve(path, argv, build_child_env(0));
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

/* Ordinary (unprivileged) launch — the common case. */
static int spawn_client(int slot, const char *path, const char *arg)
{
	return spawn_client_priv(slot, path, arg, 0);
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
static int free_slot(void);   /* defined just below; used by the launch-socket handler */

/* ---- launch socket: a non-window process (e.g. `open` in a terminal) asks us to start an app ----
 * Bind the well-known AF_UNIX path + listen. This is how the SAME "open" works from the command
 * line as from the UI: the desktop is the launcher (macOS WindowServer/launchd style). */
static void setup_spawn_socket(void)
{
	unlink(NW_SPAWN_SOCK);                         /* clear a stale socket from a previous run */
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, NW_SPAWN_SOCK, sizeof sa.sun_path - 1);
	if (bind(fd, (struct sockaddr *) &sa, sizeof sa) < 0 || listen(fd, 8) < 0) { close(fd); return; }
	set_cloexec(fd); set_nonblock(fd);
	g_spawnfd = fd;
}

/* Drain all pending launch requests. Each connection carries "cmd\0arg"; resolve `cmd` like the
 * Run dialog and spawn a new window client with `arg` (the file) as argv[1]. */
static void handle_spawn_conns(void)
{
	if (g_spawnfd < 0) return;
	int c;
	while ((c = accept(g_spawnfd, 0, 0)) >= 0) {
		set_nonblock(c);
		char buf[640]; int got = 0, r;
		while (got < (int) sizeof buf - 1 && (r = (int) read(c, buf + got, sizeof buf - 1 - got)) > 0)
			got += r;
		close(c);
		if (got <= 0) continue;
		buf[got] = 0;
		/* request = "cmd\0arg\0mode": cmd resolved like Run; arg = argv[1]; mode = "elevate"
		 * means run as root — the COMPOSITOR pops its modal auth dialog, collects the admin
		 * password itself, and (on success) launches via nanosu. The client never sees the password. */
		const char *cmd = buf;
		int cl = (int) strlen(cmd);
		const char *arg  = (cl + 1 <= got) ? buf + cl + 1 : "";
		int al = (int) strlen(arg);
		const char *mode = (cl + 1 + al + 1 <= got) ? buf + cl + 1 + al + 1 : "";
		if (mode[0] == 'e') {                              /* "elevate" -> system auth dialog */
			nw_auth_begin(&S, cmd, arg);
			continue;
		}
		char path[256];
		int slot = free_slot();
		if (slot >= 0 && resolve_cmd(cmd, path, sizeof path))
			spawn_client(slot, path, arg[0] ? arg : 0);
	}
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
	int wheel = 0;                                   /* wheel ticks accumulated for this report */
	struct input_event ev[32];
	int n;
	while ((n = (int) read(fd, ev, sizeof ev)) > 0) {
		int cnt = n / (int) sizeof(struct input_event);
		for (int i = 0; i < cnt; i++) {
			struct input_event *e = &ev[i];
			if (e->type == EV_REL) {
				if (e->code == REL_X) cx += e->value;
				else if (e->code == REL_Y) cy += e->value;
				else if (e->code == REL_WHEEL) wheel += e->value;
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
				nw_pointer(&S, cx, cy, btn, wheel);
				wheel = 0;                           /* one scroll delta per report */
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
		while (nw_decoder_next(&cl_dec[slot], &p, end)) {
			if (cl_dec[slot].overflow)             /* payload exceeded the buffer: drop it, never
			                                        * feed a partially-read payload to commit_rect
			                                        * (which would read past the buffer into heap) */
				continue;
			nw_client_msg(&S, slot, &cl_dec[slot].msg, cl_dec[slot].payload);
		}
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
		/* A resize nulls w->buf/frame but leaves the old allocations here — free them before
		 * reallocating at the new size, or they leak (and the pointer is overwritten). */
		if (g_winbuf[slot])   { free(g_winbuf[slot]);   g_winbuf[slot] = 0; }
		if (g_winframe[slot]) { free(g_winframe[slot]); g_winframe[slot] = 0; }
		if (g_winbackdrop[slot]) { free(g_winbackdrop[slot]); g_winbackdrop[slot] = 0; }
		g_winbuf[slot] = (uint32_t *) malloc((size_t) w->cw * w->ch * 4);
		if (g_winbuf[slot]) {
			/* Pre-fill with the window material (light/dark per the title's dark flag) instead of
			 * black, so a window larger than the client's painted content shows clean window
			 * background where the client hasn't drawn — like Windows erasing to the class
			 * background brush on resize. Client commits overwrite the painted region; an app that
			 * re-lays-out (NetSurf) fills the rest, one that ignores resize keeps clean margins. */
			uint32_t mat = (w->title[0] == '\x01') ? 0x0f121fu : 0xf8fbffu;
			size_t npx = (size_t) w->cw * w->ch;
			for (size_t p = 0; p < npx; p++) g_winbuf[slot][p] = mat;
		}
		w->buf = g_winbuf[slot];
		/* the cached frame: chrome (title bar + border) around the content */
		int fw = w->cw + 2 * NW_BORDER, fh = NW_TITLEBAR_H + w->ch + NW_BORDER;
		g_winframe[slot] = (uint32_t *) malloc((size_t) fw * fh * 4);
		if (g_winframe[slot]) memset(g_winframe[slot], 0, (size_t) fw * fh * 4);
		w->frame = g_winframe[slot];
		w->frame_dirty = 1;            /* render it on the next compose */
		/* per-window lo-res backdrop cache, sized to the worst-case cache_rect (full screen / F) */
		g_winbackdrop[slot] = (uint32_t *) malloc((size_t) g_bdc.lo_cap * 4);
		w->bd_blur = g_winbackdrop[slot];
		w->bd_lw = w->bd_lh = 0; w->bd_dirty = 1;
		w->bd_rect = (nw_rect){0,0,0,0};
	}
	for (int i = 0; i < NW_MAX_WINDOWS; i++)
		if (!S.win[i].used) {
			if (g_winbuf[i])   { free(g_winbuf[i]);   g_winbuf[i] = 0; }
			if (g_winframe[i]) { free(g_winframe[i]); g_winframe[i] = 0; }
			if (g_winbackdrop[i]) { free(g_winbackdrop[i]); g_winbackdrop[i] = 0; }
		}
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
/* Refresh the menu-bar clock ("HH:MM", UTC from the realtime clock). Damages the bar only when
 * the string changes (once a minute), so it costs nothing between ticks. */
static void update_clock(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return;
	long sec = ts.tv_sec;
	int h = (int) ((sec / 3600) % 24), m = (int) ((sec / 60) % 60), s = (int) (sec % 60);
	char c[12]; int suffix = 0;
	if (g_set.clock_24h) {
		if (g_set.clock_seconds) snprintf(c, sizeof c, "%02d:%02d:%02d", h, m, s);
		else                     snprintf(c, sizeof c, "%02d:%02d", h, m);
	} else {
		int hh = h % 12; if (hh == 0) hh = 12; suffix = (h >= 12);
		if (g_set.clock_seconds) snprintf(c, sizeof c, "%d:%02d:%02d %s", hh, m, s, suffix ? "PM" : "AM");
		else                     snprintf(c, sizeof c, "%d:%02d %s", hh, m, suffix ? "PM" : "AM");
	}
	if (strcmp(S.clock, c) != 0) {
		for (int j = 0; c[j] || S.clock[j]; j++) { S.clock[j] = c[j]; if (!c[j]) break; }
		S.dirty = 1;            /* time changed -> recompose so the bar clock updates */
	}
}

/* Cursor save-under: the cursor is baked into the OFF-SCREEN scene buffer just for the blit, then
 * removed again, so g_scene stays cursor-free but every framebuffer write already contains the
 * cursor — there is never a cursor-absent moment on the live LFB (that gap, between erasing the old
 * cursor and drawing the new one over a freshly-blitted scene region, was the "cursor flicker"
 * during window drags and on a continuously-repainting window like the browser). */
static uint32_t g_cur_save[NW_CURSOR_W * NW_CURSOR_H];
static void cursor_region(int *x, int *y, int *w, int *h)
{
	*x = S.cursor_x; *y = S.cursor_y; *w = NW_CURSOR_W; *h = NW_CURSOR_H;
	if (*x < 0) { *w += *x; *x = 0; }
	if (*y < 0) { *h += *y; *y = 0; }
	if (*x + *w > (int) g_xres) *w = (int) g_xres - *x;
	if (*y + *h > (int) g_yres) *h = (int) g_yres - *y;
}
static void cursor_save_scene(void)   /* g_scene[cursor box] -> g_cur_save */
{
	int x, y, w, h; cursor_region(&x, &y, &w, &h);
	for (int r = 0; r < h; r++)
		memcpy(g_cur_save + (size_t) r * NW_CURSOR_W,
		       g_scene + (size_t) (y + r) * g_xres + x, (size_t) w * 4);
}
static void cursor_restore_scene(void)  /* g_cur_save -> g_scene[cursor box] */
{
	int x, y, w, h; cursor_region(&x, &y, &w, &h);
	for (int r = 0; r < h; r++)
		memcpy(g_scene + (size_t) (y + r) * g_xres + x,
		       g_cur_save + (size_t) r * NW_CURSOR_W, (size_t) w * 4);
}

#ifdef NWM_GL
/* GPU-native present path (Task 10): refresh the per-window frame caches, then let the GL backend
 * composite the desktop ON THE GPU (wallpaper + window textures + GPU glass blur + CPU chrome
 * overlay + cursor) and scan it out via GL/KMS — no CPU scene composite, no /dev/fb0 blit. The whole
 * frame is redrawn each present (there is no cheap damage/overlay on a GPU-swapped buffer). Any
 * GL/KMS failure disables the backend and reverts to the CPU compositor for the session. */
static void present_gl(void)
{
	if (!g_own)
		return;
	if (!S.dirty && S.cursor_x == g_prev_cx && S.cursor_y == g_prev_cy)
		return;                                  /* nothing changed */
	int scene_dirty = S.dirty;                   /* 0 => bare cursor move: reuse the scene, cheap present */
	if (S.dirty) {
		nw_render_dirty_frames(&S);              /* refresh any window whose content/focus changed */
		S.frame_ctr++;
		int dx, dy, dw, dh;
		nw_take_damage(&S, &dx, &dy, &dw, &dh);  /* consume it (the GPU redraws the whole frame) */
		S.dirty = 0;
	}
	/* A drag/resize is live → the panel/taskbar can't change: nw_gl_frame skips the chrome
	 * re-render+upload. Combined with per-window & per-wallpaper upload gating, a drag frame
	 * uploads zero texture bytes. */
	int interacting = (S.drag_win >= 0 || S.resize_win >= 0);

	/* Frame-time telemetry (always on, serial): a dragged frame on TCG+virgl must stay < 25 ms. */
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int rc = nw_gl_frame(&S, &g_wall_surf, scene_dirty, interacting);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (rc != 0) {
		printf("nwm: GL backend disabled, CPU fallback\n");
		/* WEDGED teardown, not the graceful one: a context that just failed a frame can hold
		 * unsignalled fences, and eglTerminate/gbm-destroy wait on those forever — the graceful
		 * path here is what froze the desktop after the fallback (Dell boot #49, QEMU repro). */
		nw_gl_shutdown_wedged();
		g_gl = 0;
		g_force_full = 1;                        /* next present() (CPU) repaints the whole screen */
		S.dirty = 1;                             /* make the CPU takeover present immediately */
		g_gl_fell_back = 1;                      /* one-shot: the first CPU frame prints a marker */
		return;
	}
	{
		/* Separate the averages by frame class: a cursor-only frame is trivially cheap and would
		 * hide the number that matters. `dirty` = full recompose (worst case: a clock tick re-renders
		 * + re-uploads chrome). `drag` = recompose while interacting (skips every upload) — the frame
		 * the user feels; its avg must stay < 25 ms on TCG+virgl. */
		long us = ((long) t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
		static long gl_n, gl_dus, gl_dn, gl_gus, gl_gn;
		gl_n++;
		if (scene_dirty) { gl_dus += us; gl_dn++; if (interacting) { gl_gus += us; gl_gn++; } }
		if (gl_n >= 30) {
			printf("nwm-gl: 30 frames | dirty %ld avg %ldms | drag %ld avg %ldms\n",
			       gl_dn, gl_dn ? gl_dus / gl_dn / 1000 : 0,
			       gl_gn, gl_gn ? gl_gus / gl_gn / 1000 : 0);
			gl_n = gl_dus = gl_dn = gl_gus = gl_gn = 0;
		}
	}
	g_prev_cx = S.cursor_x; g_prev_cy = S.cursor_y;
}
#endif /* NWM_GL */

static void present(void)
{
#ifdef NWM_GL
	if (g_gl) { present_gl(); return; }
#endif
	if (!g_own)
		return;                                  /* another VT owns the framebuffer — never touch it */
#ifdef NWM_GL
	if (g_gl_fell_back) {
		/* One-shot proof the event loop reached the CPU present path after the GL teardown —
		 * pulled off the stick by `make i915-log` (Dell boot #49: desktop went inert after the
		 * fallback and there was no way to tell a frozen loop from an unscanned fb0). Printed on
		 * ANY present (even a bare cursor move), before the no-change early-out. */
		printf("nwm: CPU fallback presenting (loop alive)\n");
		g_gl_fell_back = 0;
	}
#endif
	if (!S.dirty && S.cursor_x == g_prev_cx && S.cursor_y == g_prev_cy)
		return;                                  /* nothing changed */
	/* Erase the old cursor (cursor-free scene at the OLD position) only when the cursor moved; the
	 * new position is drawn flicker-free below by baking the cursor into the scene before blitting. */
	if (g_prev_cx >= 0 && (g_prev_cx != S.cursor_x || g_prev_cy != S.cursor_y))
		blit_scene(g_prev_cx, g_prev_cy, NW_CURSOR_W, NW_CURSOR_H);
	if (S.dirty) {
#if NWM_PROFILE
		long pf_t0 = pf_now_ns();
#endif
		nw_render_dirty_frames(&S);              /* refresh any window whose content/focus changed */
		int dx, dy, dw, dh;
		int have = nw_peek_damage(&S, &dx, &dy, &dw, &dh);
		if (g_force_full) { have = 0; g_force_full = 0; }   /* after a VT acquire: full recompose + blit */
		if (have) {                                /* recompose only the damage region */
			nw_surface_clip(&g_scene_surf, dx, dy, dw, dh);
			nw_surface_clip(&g_scratch_surf, dx, dy, dw, dh);   /* render windows only there too */
		} else {
			nw_surface_noclip(&g_scene_surf);
			nw_surface_noclip(&g_scratch_surf);
		}
		S.frame_ctr++;
		g_bdc.frame_ctr = S.frame_ctr;
		g_bdc.drag_win  = S.drag_win;
		g_bdc.rebuild_budget = 2;   /* NW_BD_REBUILD_K: max non-priority fresh rebuilds/frame */
		g_bdc.bd = g_blur_on ? &g_bd_surf : 0;   /* blur disabled -> classic flat-tint glass */
		nw_compose_scene(&S, &g_scene_surf, &g_scratch_surf, &g_wall_surf, &g_bdc);
		nw_surface_noclip(&g_scene_surf);
		nw_surface_noclip(&g_scratch_surf);
		nw_take_damage(&S, &dx, &dy, &dw, &dh);  /* consume it */
		/* Bake the cursor into the off-screen scene so the damage blit already carries it (the LFB
		 * never shows a cursor-absent region), blit damage + the cursor box, restore scene clean. */
		cursor_save_scene();
		nw_draw_cursor(&g_scene_surf, S.cursor_x, S.cursor_y);
		if (have)
			blit_scene(dx, dy, dw, dh);          /* only the changed region (incl. baked cursor) */
		else
			blit_scene(0, 0, (int) g_xres, (int) g_yres);   /* first frame / fallback */
		blit_scene(S.cursor_x, S.cursor_y, NW_CURSOR_W, NW_CURSOR_H);   /* cursor if outside damage */
		cursor_restore_scene();
		S.dirty = 0;
#if NWM_PROFILE
		pf_record(pf_now_ns() - pf_t0);
#endif
		g_prev_cx = S.cursor_x; g_prev_cy = S.cursor_y;
		return;
	}
	/* Not dirty — only the cursor moved. Bake it into the scene, blit the cursor box (which now
	 * carries the cursor in one memcpy), restore. The old position was erased at the top. */
	cursor_save_scene();
	nw_draw_cursor(&g_scene_surf, S.cursor_x, S.cursor_y);
	blit_scene(S.cursor_x, S.cursor_y, NW_CURSOR_W, NW_CURSOR_H);
	cursor_restore_scene();
	g_prev_cx = S.cursor_x; g_prev_cy = S.cursor_y;
}

/* Wallpaper-load diagnostics: on the GL build every step goes to the lossless gldiag trace (the
 * greeter relogin bug hid a SIGSEGV exactly in this window — between "refresh_wallpaper..." and
 * "wallpaper rendered" — so each stage names itself); a plain fbdev build compiles them away. */
#ifdef NWM_GL
#define WALLDIAG(...) glkms_diag(__VA_ARGS__)
#else
#define WALLDIAG(...) do {} while (0)
#endif

/* Linearly interpolate two 0x00RRGGBB pixels, per channel; t is the weight of `b` in 16.16
 * (0 -> a, 65536 -> b). The building block of bilinear sampling. */
static uint32_t lerp_px(uint32_t a, uint32_t b, unsigned t)
{
	int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
	int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
	int r  = ar + (((br - ar) * (int) t) >> 16);
	int g  = ag + (((bg - ag) * (int) t) >> 16);
	int bl = ab + (((bb - ab) * (int) t) >> 16);
	return ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) bl;
}

/* Load the branded wallpaper.png, decode it in-process, and cover-fit it (preserve aspect, crop the
 * overflow, centered) to the actual screen size `w`x`h` — so it fills ANY resolution the firmware
 * gave us instead of needing a build-time-sized raw. Sampling is BILINEAR (4-tap), so scaling is
 * smooth instead of the blocky nearest-neighbour look. Returns 1 on success; the caller falls back
 * to the procedural gradient otherwise. */
static int load_wallpaper(uint32_t *dst, unsigned w, unsigned h)
{
	int fd = open("/disks/main/nanos/share/wallpaper.png", O_RDONLY);
	if (fd < 0) return 0;
	/* Slurp the file. */
	uint8_t *file = 0; unsigned cap = 0, got = 0;
	for (;;) {
		if (got == cap) { cap = cap ? cap * 2 : (1u << 20); uint8_t *n = (uint8_t *) realloc(file, cap); if (!n) { free(file); close(fd); return 0; } file = n; }
		int r = read(fd, file + got, cap - got);
		if (r < 0) { WALLDIAG("nwm-gl: wallpaper read err after %u bytes — gradient fallback\n", got); free(file); close(fd); return 0; }
		if (r == 0) break;
		got += (unsigned) r;
	}
	close(fd);
	WALLDIAG("nwm-gl: wallpaper slurped %u bytes; decode...\n", got);

	int iw = 0, ih = 0;
	uint32_t *src = png_decode(file, got, &iw, &ih);
	free(file);
	if (!src || iw <= 0 || ih <= 0) {
		/* "chunk crc (corrupt read)" here = the bytes read off the stick don't match what the
		 * encoder wrote — a storage-path corruption caught red-handed, not a bad asset. */
		WALLDIAG("nwm-gl: wallpaper png FAILED (%s) — gradient fallback\n", png_last_error());
		free(src);
		return 0;
	}
	WALLDIAG("nwm-gl: wallpaper decoded %dx%d; scale...\n", iw, ih);
	for (size_t i = 0; i < (size_t) iw * ih; i++) src[i] &= 0x00ffffffu;  /* opaque wallpaper: drop alpha */

	/* Cover-fit: source pixels per screen pixel = min(iw/w, ih/h) in 16.16 fixed point (the smaller
	 * step zooms in to fill, cropping the other axis); center the sampled region. Each destination
	 * pixel samples the 4 source texels around its fractional position and blends them (bilinear). */
	uint64_t stepx = ((uint64_t) iw << 16) / w;
	uint64_t stepy = ((uint64_t) ih << 16) / h;
	uint64_t step = stepx < stepy ? stepx : stepy;
	long sx0 = (long) (((uint64_t) iw << 16) - (uint64_t) w * step) / 2;
	long sy0 = (long) (((uint64_t) ih << 16) - (uint64_t) h * step) / 2;
	for (unsigned y = 0; y < h; y++) {
		long fy = sy0 + (long) ((uint64_t) y * step);
		long iy = fy >> 16; unsigned ty = (unsigned) (fy & 0xFFFF);
		long iy0 = iy < 0 ? 0 : (iy >= ih ? ih - 1 : iy);
		long iy1 = iy + 1 < 0 ? 0 : (iy + 1 >= ih ? ih - 1 : iy + 1);
		const uint32_t *r0 = src + (size_t) iy0 * iw;
		const uint32_t *r1 = src + (size_t) iy1 * iw;
		uint32_t *drow = dst + (size_t) y * w;
		for (unsigned x = 0; x < w; x++) {
			long fx = sx0 + (long) ((uint64_t) x * step);
			long ix = fx >> 16; unsigned tx = (unsigned) (fx & 0xFFFF);
			long ix0 = ix < 0 ? 0 : (ix >= iw ? iw - 1 : ix);
			long ix1 = ix + 1 < 0 ? 0 : (ix + 1 >= iw ? iw - 1 : ix + 1);
			uint32_t top = lerp_px(r0[ix0], r0[ix1], tx);
			uint32_t bot = lerp_px(r1[ix0], r1[ix1], tx);
			drow[x] = lerp_px(top, bot, ty);
		}
	}
	free(src);
	WALLDIAG("nwm-gl: wallpaper scaled to %ux%u\n", w, h);
	return 1;
}

/* Fill g_wall per the wallpaper setting: branded PNG (fallback gradient), procedural gradient,
 * or a solid colour. Called at boot and on every settings reload. */
static void refresh_wallpaper(void)
{
	if (g_set.wallpaper == NW_WALL_GRADIENT) {
		WALLDIAG("nwm-gl: wallpaper mode=gradient\n");
		nw_render_wallpaper(&g_wall_surf);
	} else if (g_set.wallpaper == NW_WALL_SOLID) {
		size_t n = (size_t) g_xres * g_yres;
		WALLDIAG("nwm-gl: wallpaper mode=solid\n");
		for (size_t i = 0; i < n; i++) g_wall[i] = g_set.wallpaper_color;
	} else {                                   /* NW_WALL_BRANDED */
		WALLDIAG("nwm-gl: wallpaper mode=branded\n");
		if (!load_wallpaper(g_wall, g_xres, g_yres))
			nw_render_wallpaper(&g_wall_surf);
	}
#ifdef NWM_GL
	if (g_gl) nw_gl_wallpaper_changed();       /* the GL backend re-uploads the wallpaper next frame */
#endif
}

/* ---- virtual-terminal ownership (Linux VT_SETMODE process mode) ----------------------------
 * nwm runs on the graphics VT (tty7). The kernel only lets the OWNER of the active VT draw the
 * framebuffer, so nwm registers as the process-mode owner and gates every fb write on g_own:
 *   - on switch-AWAY the kernel sends relsig (SIGUSR1): stop drawing + ack with VT_RELDISP so the
 *     incoming text VT can repaint;
 *   - on switch-TO the kernel sends acqsig (SIGUSR2): resume drawing + force a full redraw.
 * A self-pipe wakes the poll() loop on either signal so the screen updates promptly. */
#define KDSETMODE   0x4B3A
#define KD_GRAPHICS 0x01
#define VT_SETMODE  0x5602
#define VT_RELDISP  0x5605
#define VT_PROCESS  0x01
#ifndef SIGUSR1
#define SIGUSR1 10
#define SIGUSR2 12
#endif
struct nw_vt_mode { unsigned char mode, waitv; short relsig, acqsig, frsig; };

static void vt_on_release(int s) {  /* SIGUSR1: kernel asks us to yield the console */
	(void) s;
	g_own = 0;
	if (g_ttyfd >= 0) ioctl(g_ttyfd, VT_RELDISP, 1);   /* ack: release granted -> kernel repaints the text VT */
	if (g_wakefd[1] >= 0) { char c = 'r'; write(g_wakefd[1], &c, 1); }
}
static void vt_on_acquire(int s) {  /* SIGUSR2: kernel handed the console back to us */
	(void) s;
	g_own = 1;
	g_force_full = 1;               /* the LFB held another VT — repaint the whole screen, not damage */
	g_prev_cx = -1;                 /* force the cursor redraw too */
	S.dirty = 1;
	if (g_wakefd[1] >= 0) { char c = 'a'; write(g_wakefd[1], &c, 1); }
}

/* Spawn the demo desktop (Terminal + Settings + Files) and draw the first frame. Runs ONCE, the
 * first time we own the console — at boot for a standalone nwm, or on the first switch to F7 for a
 * VT-managed one. Deferring it keeps boot light (no GUI apps until someone looks at the desktop). */
static void start_desktop(void)
{
	if (g_started) return;
	g_started = 1;
	spawn_client(0, NWTERM_PATH, 0);          /* the Nano OS demo desktop: Terminal + Settings */
	spawn_client(1, NWSET_PATH, 0);
	spawn_client(2, NWEXP_PATH, 0);           /* Files spawned last -> on top + focused */
	S.dirty = 1;
	present();                                /* first frame: desktop + cursor */
}

#ifdef NWM_GL
void nx_bind_std_streams(void);   /* nx_stream_bridge.c: bind stdout/stderr from libc.ndl __imp_ slots */
#endif

int main(void)
{
#ifdef NWM_GL
	nx_bind_std_streams();   /* before any printf: Mesa/libdrm C++ TUs reference the stream globals */
	/* Persist every nwm/glkms/Mesa diagnostic: stdout/stderr point at tty7 (a graphics VT —
	 * invisible, gone at power-off), which is exactly how Dell boot #47 lost the name of the GL
	 * call that failed. TEE (not redirect) through a logger child so the markers still reach
	 * tty7/serial — smoke-virtio-gpu-gl's oracle greps the serial log for "nwm: GL compositor
	 * active" — while a copy lands on the stick for `make i915-log`. O_APPEND: the greeter
	 * respawns nwm per login and every session must survive; image64-gl ships empty logs.
	 * Silently keep plain tty7 if the logs dir is absent (dev images). */
	{
		int lfd = open("/disks/main/nanos/logs/nwm.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
		if (lfd >= 0) {
			int tp[2];
			if (pipe(tp) == 0) {
				int lpid = fork();
				if (lpid == 0) {
					/* logger: copy nwm's (and its clients') output to BOTH destinations.
					 * fd 1 is still the original tty7 here — dup2 below only changes the parent. */
					close(tp[1]);
					char lbuf[512];
					int ln;
					while ((ln = (int) read(tp[0], lbuf, sizeof lbuf)) > 0) {
						write(1, lbuf, (size_t) ln);
						write(lfd, lbuf, (size_t) ln);
					}
					_exit(0);
				}
				close(tp[0]);
				if (lpid > 0) {
					dup2(tp[1], 1);
					dup2(tp[1], 2);
				}
				close(tp[1]);
			}
			close(lfd);
			printf("nwm: ===== run (GL build) =====\n");
		}
	}
	/* LOSSLESS startup trace (Dell GL freeze / 3x-login): each nwm-gl run is a fresh process the
	 * greeter execs, so tag it by pid; the death point of a run that never reaches the loop lands in
	 * gldiag.txt with a synchronous write (survives the reboot that swallows nwm.txt). Remove with
	 * the other DIAG. */
	glkms_diag("nwm-gl: START pid=%d\n", getpid());
#endif
	int fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0) { printf("nwm: no /dev/fb0\n"); return 1; }
	/* fb0 geometry, VALIDATED with a bounded retry. Previously var/fix were read into UNINITIALIZED
	 * stack with the ioctl return value ignored: if /dev/fb0 was not yet reporting a usable mode when
	 * an early greeter-spawned nwm raced the i915 driver settling its display power domains, the
	 * garbage/degenerate dimensions drove the ~50 MB geometry-sized mallocs and memsets below straight
	 * into a SIGSEGV — the process died before "fb0 ok", the greeter relogged, and it repeated several
	 * times until the fb settled. Zero the structs, check the ioctls, and require a sane mode; retry
	 * (~4 s budget) so nwm waits out the settle instead of crash-looping the login. */
	struct fb_var var; struct fb_fix fix;
	int fb_ok = 0;
	for (int tries = 0; tries < 200; tries++) {
		memset(&var, 0, sizeof var);
		memset(&fix, 0, sizeof fix);
		int rv = ioctl(fbfd, FBIOGET_VSCREENINFO, &var);
		int rf = ioctl(fbfd, FBIOGET_FSCREENINFO, &fix);
		if (rv == 0 && rf == 0 &&
		    var.xres >= 320 && var.xres <= 16384 &&
		    var.yres >= 200 && var.yres <= 16384 &&
		    fix.line_length >= var.xres * 4 &&
		    fix.smem_len >= (uint32_t) fix.line_length * var.yres) {
			fb_ok = 1;
			break;
		}
#ifdef NWM_GL
		glkms_diag("nwm-gl: fb0 not ready (rv=%d rf=%d xres=%u yres=%u pitch=%u smem=%u) retry=%d\n",
		           rv, rf, var.xres, var.yres, fix.line_length, fix.smem_len, tries);
#endif
		poll(0, 0, 20);   /* 20 ms; up to ~4 s total for the i915 fb backing to settle */
	}
	if (!fb_ok) { printf("nwm: /dev/fb0 never reported a usable mode\n"); return 1; }
	g_xres = var.xres; g_yres = var.yres; g_pitch = fix.line_length;
#ifdef NWM_GL
	glkms_diag("nwm-gl: fb0 mode %ux%u pitch=%u smem=%u — mmap...\n", g_xres, g_yres, g_pitch, fix.smem_len);
#endif
	g_fb = (uint8_t *) mmap(0, fix.smem_len, 3, 1, fbfd, 0);
	if (g_fb == (uint8_t *) -1 || !g_fb) { printf("nwm: fb mmap failed\n"); return 1; }
	size_t fbpx = (size_t) g_xres * g_yres * 4;
	g_scene   = (uint32_t *) malloc(fbpx);
	g_scratch = (uint32_t *) malloc(fbpx);
	g_wall    = (uint32_t *) malloc(fbpx);
	if (!g_scene || !g_scratch || !g_wall) { printf("nwm: no memory for compositor buffers\n"); return 1; }
	memset(g_scene, 0, fbpx); memset(g_scratch, 0, fbpx);   /* never composite/blit malloc garbage */
	g_scene_surf.px = g_scene; g_scene_surf.w = (int) g_xres; g_scene_surf.h = (int) g_yres;
	g_scene_surf.stride = (int) g_xres; nw_surface_noclip(&g_scene_surf);
	g_scratch_surf.px = g_scratch; g_scratch_surf.w = (int) g_xres; g_scratch_surf.h = (int) g_yres;
	g_scratch_surf.stride = (int) g_xres; nw_surface_noclip(&g_scratch_surf);
	g_wall_surf.px = g_wall; g_wall_surf.w = (int) g_xres; g_wall_surf.h = (int) g_yres;
	g_wall_surf.stride = (int) g_xres; nw_surface_noclip(&g_wall_surf);
	g_bd   = (uint32_t *) malloc(fbpx);
	int lopx = ((int) g_xres / NW_BD_DOWNSAMPLE + 1) * ((int) g_yres / NW_BD_DOWNSAMPLE + 1);
	g_bdlo = (uint32_t *) malloc((size_t) lopx * 4);
	if (!g_bd || !g_bdlo) { printf("nwm: no memory for backdrop buffers\n"); return 1; }
	memset(g_bd, 0, fbpx);
	g_bd_surf.px = g_bd; g_bd_surf.w = (int) g_xres; g_bd_surf.h = (int) g_yres;
	g_bd_surf.stride = (int) g_xres; nw_surface_noclip(&g_bd_surf);
	g_bdc.bd = &g_bd_surf; g_bdc.lo = g_bdlo; g_bdc.lo_cap = lopx;
	g_bdc.factor = NW_BD_DOWNSAMPLE; g_bdc.radius = NW_BD_BLUR_RADIUS; g_bdc.passes = NW_BD_BLUR_PASSES;
#ifdef NWM_GL
	glkms_diag("nwm-gl: compositor buffers allocated; apply_settings...\n");
#endif
	apply_settings();                          /* load /nanos/config/settings.yaml (or defaults) */
#ifdef NWM_GL
	glkms_diag("nwm-gl: settings applied; refresh_wallpaper...\n");
#endif
	refresh_wallpaper();                       /* render the wallpaper per the chosen mode */
#ifdef NWM_GL
	glkms_diag("nwm-gl: wallpaper rendered\n");
#endif
	g_fb_surf.px = (uint32_t *) g_fb; g_fb_surf.w = (int) g_xres; g_fb_surf.h = (int) g_yres;
	g_fb_surf.stride = (int) (g_pitch / 4); nw_surface_noclip(&g_fb_surf);

#ifdef NWM_GL
	/* Task 10: try the GL ES present backend (card0 + GBM + EGL). NWM_NO_GL=1 or no DRM node
	 * (plain QEMU) => stay on the CPU fb0 path. The scene is composed identically either way. */
	glkms_diag("nwm-gl: fb0 %ux%u ok; nw_gl_init...\n", g_xres, g_yres);
	if (!getenv("NWM_NO_GL") && nw_gl_init((int) g_xres, (int) g_yres) == 0) {
		g_gl = 1;
		glkms_diag("nwm-gl: nw_gl_init OK (g_gl=1), entering present loop\n");
		nw_gl_build_cursor();
		nw_gl_set_radius(g_set.corner_radius);
		printf("nwm: GL compositor active\n");
	} else {
		printf("nwm: GL compositor unavailable, CPU compositor active\n");
	}
#endif

	int in0 = open("/dev/input0", O_RDONLY | O_NONBLOCK);
	int in1 = open("/dev/input1", O_RDONLY | O_NONBLOCK);
	set_cloexec(fbfd); set_cloexec(in0); set_cloexec(in1);

	signal(SIGPIPE, SIG_IGN);                 /* a client exiting must not kill the server */
	/* The compositor must never be job-control-stopped: a windowed terminal's shell may grab
	 * the controlling tty (tcsetpgrp), which would otherwise SIGTTOU/SIGTTIN/SIGTSTP us. */
#ifndef SIGTSTP
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#endif
	signal(SIGTTOU, SIG_IGN); signal(SIGTTIN, SIG_IGN); signal(SIGTSTP, SIG_IGN);

	/* Become the graphics VT (tty7) owner: KD_GRAPHICS (the kernel stops drawing its text console
	 * here) + VT_SETMODE process mode, so Ctrl+Alt+Fn switching hands the framebuffer between us and
	 * the text consoles cooperatively. g_own starts 0 — we do not touch the fb until the kernel
	 * switches to F7 and sends the acquire signal. A self-pipe lets the signal wake the poll loop. */
	g_ttyfd = open("/dev/tty7", O_RDWR);
	if (g_ttyfd >= 0) {
		set_cloexec(g_ttyfd);
		if (pipe(g_wakefd) == 0) {
			set_cloexec(g_wakefd[0]); set_cloexec(g_wakefd[1]);
			/* The self-pipe is a poll() wakeup ONLY: the loop drains it with `while (read() > 0)`,
			 * which needs the read end non-blocking (else it blocks on the empty pipe and the whole
			 * event loop stalls). The write end is non-blocking too so the signal handler's poke can
			 * never block if the pipe is momentarily full. */
			set_nonblock(g_wakefd[0]); set_nonblock(g_wakefd[1]);
		}
		signal(SIGUSR1, vt_on_release);
		signal(SIGUSR2, vt_on_acquire);
		ioctl(g_ttyfd, KDSETMODE, KD_GRAPHICS);
		struct nw_vt_mode vm; vm.mode = VT_PROCESS; vm.waitv = 0;
		vm.relsig = SIGUSR1; vm.acqsig = SIGUSR2; vm.frsig = 0;
		ioctl(g_ttyfd, VT_SETMODE, &vm);
	} else {
		g_own = 1;   /* no VT (e.g. launched standalone): own the fb outright, legacy behaviour */
	}

	for (int i = 0; i < NW_MAX_CLIENTS; i++) { cl_req[i] = cl_evt[i] = -1; }
	nw_server_init(&S, (int) g_xres, (int) g_yres);
	setup_spawn_socket();   /* the launch channel for `open` (and any non-window launcher) */

	/* When we own a graphics VT (tty7) the kernel does not text-draw it, so termmode is unneeded;
	 * and we must do NOTHING (no console mode change, no GUI clients, no fb writes) until the first
	 * acquire — otherwise we'd spawn three GUI apps at boot and flip the ACTIVE text VT's line
	 * discipline, racing every other console's login. start_desktop() runs once, when first owned. */
	if (g_own) {                              /* standalone (startx-style): own the console now */
		termmode(1);                          /* silence the kernel console echo-draw */
		start_desktop();
	}

	for (;;) {
		struct pollfd pfd[4 + NW_MAX_CLIENTS * 2];
		int n = 0;
		pfd[n].fd = in0; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
		pfd[n].fd = in1; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
		if (g_wakefd[0] >= 0) { pfd[n].fd = g_wakefd[0]; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
		if (g_spawnfd >= 0)   { pfd[n].fd = g_spawnfd;   pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
		for (int i = 0; i < NW_MAX_CLIENTS; i++) {
			if (cl_req[i] < 0)
				continue;
			pfd[n].fd = cl_req[i]; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
			if (nw_outq_pending(&S, i) > 0) {
				pfd[n].fd = cl_evt[i]; pfd[n].events = POLLOUT; pfd[n].revents = 0; n++;
			}
		}
		/* A modest timeout (not -1): a VT acquire/release arrives as a signal, and a self-pipe poke
		 * may race the poll re-arm, so wake periodically to service g_own changes + the menu clock.
		 * present() early-outs when nothing changed, so an idle wake is cheap. */
		poll(pfd, n, 200);

		/* COALESCE: drain every input + request before drawing. The (int) cast matches the other
		 * drain loops: libc's read() returns a 32-bit int, but <unistd.h> prototypes it ssize_t, so an
		 * uncast -1 (EAGAIN) widens to 0xFFFFFFFF (> 0) and the loop would spin forever. */
		if (g_wakefd[0] >= 0) { char wb[16]; while ((int) read(g_wakefd[0], wb, sizeof wb) > 0) {} }  /* drain VT-signal pokes */
		if (g_own && !g_started)              /* first switch to F7: bring up the desktop now */
			start_desktop();
		drain_keyboard(in0);
		drain_mouse(in1);
		handle_spawn_conns();   /* launch requests from `open` (terminal) -> spawn the app */
		for (int i = 0; i < NW_MAX_CLIENTS; i++)
			if (cl_req[i] >= 0 && drain_client(i) == 0)
				disconnect(i);

		reconcile_buffers();
		if (g_ttyfd < 0)
			termmode(1);   /* standalone only: keep the (text) console raw/no-echo. On a graphics VT
			                * the kernel never text-draws, so termmode is unneeded — and flipping the
			                * ACTIVE VT's line discipline from here would corrupt a text console. */

		if (S.want_shutdown) {                 /* Shutdown button: power the machine off */
			termmode(0);
			if (g_own) memset(g_fb, 0, (size_t) g_pitch * g_yres);
			sys_poweroff();                    /* does not return */
		}
		if (S.want_quit)                       /* Quit button: leave the desktop */
			break;

		if (S.want_reload) {                   /* Settings app changed a preference -> apply live */
			S.want_reload = 0;
			apply_settings();
			nw_font_reload_from_settings();    /* UI font may have changed -> reload it live */
			refresh_wallpaper();               /* wallpaper mode/colour may have changed */
			for (int i = 0; i < NW_MAX_WINDOWS; i++)
				if (S.win[i].used) S.win[i].frame_dirty = 1;   /* accent is baked into the focus dot */
			S.dmg = 1; S.dmg_x0 = 0; S.dmg_y0 = 0;             /* full repaint: theme touches everything */
			S.dmg_x1 = (int) g_xres; S.dmg_y1 = (int) g_yres;
			S.dirty = 1;
		}

		char cmd[NW_RUN_MAX];                  /* Run dialog (Super+R): launch the typed app */
		if (nw_run_take_spawn(&S, cmd, sizeof cmd)) {
			char path[256];
			int slot = free_slot();
			const char *arg = S.spawn_has_arg ? S.run_arg : 0;   /* open-with: argv[1] */
			if (slot >= 0 && resolve_cmd(cmd, path, sizeof path))
				spawn_client(slot, path, arg);
		}

		/* The system auth dialog was submitted: launch the target as root (nanosu verifies the
		 * password the compositor collected). On a wrong password nanosu just exits — nothing runs. */
		char acmd[NW_RUN_MAX], aarg[NW_RUN_MAX], apass[NW_AUTH_MAX];
		if (nw_auth_take(&S, acmd, aarg, apass, NW_RUN_MAX)) {
			char path[256];
			int slot = free_slot();
			if (slot >= 0 && resolve_cmd(acmd, path, sizeof path))
				spawn_client_priv(slot, path, aarg[0] ? aarg : 0, apass[0] ? apass : 0);
			for (int i = 0; i < (int) sizeof apass; i++) apass[i] = 0;   /* scrub the local copy */
		}

		update_clock();   /* refresh the menu-bar clock; damages the bar when the minute ticks */
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
	if (g_own) memset(g_fb, 0, (size_t) g_pitch * g_yres);   /* clear the desktop on the way out */
	printf("nwm: exit\n");
	return 0;
}
