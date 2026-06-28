/*
 * nwm_core.h — the PURE heart of the NanWM compositor: window list, z-order, focus,
 * hit-testing, title-bar dragging, the macOS-style Super shortcut + clipboard, the US
 * scancode keymap, and a per-client output byte-ring. No I/O, no allocation, no libc state
 * — every buffer is caller-owned, so the whole thing is host-tested by feeding synthetic
 * input/requests and inspecting the bytes queued for each client.
 *
 * The I/O shell (user/nwm/nwm.c, Phase 3) is the only part that touches /dev/fb0,
 * /dev/input*, poll and fork/exec; it calls into here and drains the output rings to pipes.
 */
#ifndef NWM_CORE_H
#define NWM_CORE_H

#include <stdint.h>
#include "nwproto.h"
#include "nw_backdrop.h"   /* nw_rect (+ tunables) for the per-window backdrop cache */

/* Limits + decoration geometry (frame = title bar on top, thin border elsewhere). */
enum {
	NW_MAX_WINDOWS = 32,
	NW_MAX_CLIENTS = 16,
	NW_BORDER      = 2,
	NW_TITLEBAR_H  = 28,    /* taller bar: icon + title on the left, — □ × controls on the right */
	NW_CLOSE       = 22,    /* the × control (rightmost), the click target */
	NW_TITLE_MAX   = 64,
	NW_CLIP_MAX    = 256,
	NW_PANEL_H     = 28,    /* top menu bar: logo + app name + menus, clock + status pills */
	NW_RADIUS      = 11,    /* window corner radius (rounded, translucent "glass" frames) */
	NW_SHADOW      = 10,    /* drop-shadow extent (px) added to a window's damage rect */
	NW_DOCK_H      = 62,    /* bottom dock height (the rounded translucent pill)            */
	NW_RUN_W       = 460,   /* the Super+R "Run" dialog box */
	NW_RUN_H       = 60,
	NW_RUN_MAX     = 120,   /* max command length typed into it */
	NW_AUTH_W      = 440,   /* the system authentication (sudo) dialog */
	NW_AUTH_H      = 188,
	NW_AUTH_MAX    = 128,   /* max password length */
	NW_AUTH_BTN_W  = 124,   /* its Authenticate / Cancel buttons */
	NW_AUTH_BTN_H  = 30,
	NW_MENU_MAX    = 512,   /* per-window menu spec bytes (NW_REQ_SET_MENU payload)        */
	NW_MENU_X0     = 30,    /* where the app menu titles start (after the logo mark)       */
	NW_MENU_ITEM_H = 24,    /* dropdown item row height                                   */
	NW_MENU_DROP_W = 200,   /* dropdown width                                             */
	NW_TASK_H      = 40,    /* bottom taskbar height (full width: Start + one button/window) */
	NW_START_W     = 92,    /* Start button width (NanoOS logo + "Start")                  */
	NW_TASK_W      = 168    /* per-window task-button width                                */
};

/* nw_menubar_hit return: a top menu index >=0, or one of these. */
enum { NW_MENU_NONE = -2, NW_MENU_LOGO = -1 };


/* Normalised /dev/input0 scancodes we special-case (bit7=extended, bits0-6=set-1). */
enum {
	NW_SC_LSUPER = 0xDB, NW_SC_RSUPER = 0xDC,   /* 0xE0 0x5B / 0x5C: the GUI (Super/Cmd) keys */
	NW_SC_LSHIFT = 0x2A, NW_SC_RSHIFT = 0x36,
	NW_SC_LCTRL  = 0x1D, NW_SC_RCTRL  = 0x9D,   /* Ctrl (right = 0xE0 0x1D normalized): drag copy */
	NW_SC_C = 0x2E, NW_SC_X = 0x2D, NW_SC_V = 0x2F, NW_SC_Q = 0x10, NW_SC_TAB = 0x0F,
	NW_SC_R = 0x13, NW_SC_ESC = 0x01, NW_SC_ENTER = 0x1C, NW_SC_BACKSP = 0x0E, NW_SC_M = 0x32
};

/* Hit-test regions. */
enum { NW_HIT_NONE = 0, NW_HIT_CONTENT = 1, NW_HIT_TITLE = 2, NW_HIT_CLOSE = 3, NW_HIT_MIN = 4,
       NW_HIT_MAX = 5, NW_HIT_RESIZE = 6 };
enum { NW_RESIZE_GRIP = 16, NW_MIN_CW = 160, NW_MIN_CH = 80 };   /* bottom-right resize grip + min content size */

/* Taskbar (bottom bar): the Start button + one button per open window. */
enum { NW_TB_NONE = 0, NW_TB_START = 1, NW_TB_TASK = 2 };

/* Window kind — drives backdrop-blur recursion scope + update priority (Phase 5). */
enum nw_win_type { NW_WIN_NORMAL = 0, NW_WIN_MENU, NW_WIN_POPUP, NW_WIN_DIALOG, NW_WIN_TOOLTIP };

struct nw_window {
	int       used;
	uint32_t  id;
	int       client;
	int       x, y;        /* frame top-left, in screen pixels                       */
	int       cw, ch;      /* content size                                           */
	uint32_t *buf;         /* content pixels (cw*ch), bound by the shell after create */
	uint32_t *frame;       /* cached frame_w*frame_h window-local render (chrome+content); the
	                        * shell allocates it. NULL = render straight to the scene (host path). */
	int       frame_dirty; /* the cached frame is stale and must be re-rendered. Set on content
	                        * commit, focus change and create; a move (x/y) does NOT set it.   */
	uint8_t   glass;       /* 1 => this window gets a blurred backdrop (default for all)      */
	uint8_t   type;        /* enum nw_win_type; default NW_WIN_NORMAL                         */
	uint32_t *bd_blur;     /* per-window LO-RES blurred backdrop cache (caller-allocated)     */
	int       bd_lw, bd_lh;/* lo-res cache dimensions                                        */
	nw_rect   bd_rect;     /* the cache_rect (screen coords) bd_blur was computed for         */
	int       bd_dirty;    /* 1 => backdrop must be rebuilt next compose                      */
	int       minimized;   /* hidden from the scene (taskbar button stays); restored from the taskbar */
	int       maximized;   /* filling the work area (between menu bar and taskbar)                    */
	int       sx, sy, scw, sch;   /* geometry saved before maximizing, restored on un-maximize        */
	char      title[NW_TITLE_MAX];
	char      menu[NW_MENU_MAX];   /* app menu spec (NW_REQ_SET_MENU); empty = no app menu */
	int       menu_len;
};

/* A byte ring holding serialized server->client events, drained to the pipe on POLLOUT.
 * Backed by a caller-owned buffer; enqueue refuses to write a partial message (it would
 * desync the client's framing) — on a full ring the owning client is marked dead instead. */
struct nw_outq {
	unsigned char *buf;
	uint32_t       cap, head, count;
};

struct nw_server {
	int screen_w, screen_h;

	struct nw_window win[NW_MAX_WINDOWS];
	int   zorder[NW_MAX_WINDOWS];   /* window indices, [0]=back .. [zn-1]=front (top) */
	int   zn;
	uint32_t next_id;
	int   focus;                    /* focused window index, or -1                    */

	int   cursor_x, cursor_y;
	int   buttons;                  /* last pointer button mask (NW_BTN_*)            */
	int   drag_win;                 /* window index being dragged, or -1              */
	int   drag_dx, drag_dy;         /* cursor - frame origin at grab                  */
	int   resize_win;               /* window index being resized by the grip, or -1  */
	int   resize_dx, resize_dy;     /* cursor - frame bottom-right at grab            */

	int   super_down, shift_down, ctrl_down;

	/* Drag-and-drop: a client started a drag (NW_REQ_DRAG_BEGIN); the compositor arbitrates it
	 * (only it knows window geometry + z-order + cursor), routing DRAG_MOTION/LEAVE to the window
	 * under the cursor and DROP — carrying the payload — to the target on button release. */
	int   dnd_active;               /* a drag is in progress                                  */
	int   dnd_src_client;           /* client that started it                                 */
	int   dnd_target;               /* window index currently under the cursor (-1 = none)    */
	char  dnd_payload[NW_CLIP_MAX]; /* the dragged data (e.g. a file path)                    */
	int   dnd_len;

	char  clip[NW_CLIP_MAX];
	int   clip_len;

	struct nw_outq out[NW_MAX_CLIENTS];
	int   client_used[NW_MAX_CLIENTS];
	int   client_dead[NW_MAX_CLIENTS];  /* output overran its ring -> shell disconnects */

	int   want_quit, want_shutdown; /* a panel button was clicked -> the shell acts       */
	int   want_reload;              /* a client asked to reload settings.yaml -> shell acts */

	/* Super+R "Run" launcher dialog (WM chrome). While open it captures the keyboard. */
	int   run_open;
	char  run_text[NW_RUN_MAX];
	int   run_len;
	char  run_cmd[NW_RUN_MAX];      /* the committed command (on Enter)                  */
	int   want_spawn;               /* shell: launch run_cmd, then clear                 */
	char  run_arg[NW_RUN_MAX];      /* optional argv[1] for the spawn (e.g. a file to open) */
	int   spawn_has_arg;            /* 1 => run_arg is set (open-with); 0 => no argument     */

	/* System authentication ("sudo") dialog — compositor-owned, modal: ANY client can request an
	 * elevated launch (NW_REQ_ELEVATE / the spawn socket); nwm presents this, captures the keyboard,
	 * collects the admin password ITSELF (the client never sees it), and on success the shell runs
	 * the target as root via nwsu. Blocks all other input while open. */
	int   auth_open;
	char  auth_cmd[NW_RUN_MAX];     /* command to launch elevated on success                */
	char  auth_arg[NW_RUN_MAX];     /* its optional argv[1]                                 */
	char  auth_pass[NW_AUTH_MAX];   /* the typed password (masked on screen, scrubbed after) */
	int   auth_passlen;
	int   auth_hover;               /* hovered button: 0 = Authenticate, 1 = Cancel, -1 none */
	int   want_elevate;             /* submit -> shell runs auth_cmd as root, then clears    */

	/* global menu bar: an open dropdown (logo or the focused app's), + hovered item */
	int   menu_open, menu_which, menu_hover;
	int   menu_from_start;          /* the open menu is the Start menu -> anchor it above the taskbar */
	char  clock[12];                /* "HH:MM[:SS]" / "H:MM AM" at the right of the bar (shell sets) */

	int   frame_ctr;                /* bumped by the shell each present() — drives fast-drag cadence */
	int   dirty;                    /* the SCENE changed -> shell recomposes it          */
	/* Accumulated scene-damage bounding box (screen px) since the last present; the shell
	 * blits only this region of the recomposed scene to the framebuffer. */
	int   dmg, dmg_x0, dmg_y0, dmg_x1, dmg_y1;
};

/* ---- lifecycle ---- */
void nw_server_init(struct nw_server *s, int screen_w, int screen_h);
void nw_client_connect(struct nw_server *s, int client, unsigned char *outbuf, uint32_t outcap);
void nw_client_disconnect(struct nw_server *s, int client);   /* drops its windows */

/* ---- input (from /dev/input1 / /dev/input0) ---- */
#ifdef __cplusplus
void nw_pointer(struct nw_server *s, int sx, int sy, int buttons, int wheel = 0);
#else
void nw_pointer(struct nw_server *s, int sx, int sy, int buttons, int wheel);
#endif
void nw_key(struct nw_server *s, unsigned char code, int down);

/* ---- client requests (already decoded by nwproto) ---- */
void nw_client_msg(struct nw_server *s, int client, const struct nw_msg *m,
                   const unsigned char *payload);

/* ---- shell helpers ---- */
/* Next used window still lacking a pixel buffer (the shell allocates cw*ch then sets ->buf).
 * Returns NULL when all windows are backed. */
struct nw_window *nw_window_needs_buffer(struct nw_server *s);
/* Drain one client's output: peek the contiguous readable span, write it, then ack n bytes. */
const unsigned char *nw_outq_peek(struct nw_server *s, int client, uint32_t *len);
void nw_outq_ack(struct nw_server *s, int client, uint32_t n);
int  nw_client_is_dead(const struct nw_server *s, int client);
uint32_t nw_outq_pending(const struct nw_server *s, int client);
/* Read the accumulated scene-damage rect WITHOUT clearing it (so present() can scissor the
 * recompose to it before consuming it). Returns 1 + the box, else 0. */
int  nw_peek_damage(const struct nw_server *s, int *x, int *y, int *w, int *h);
/* Take the accumulated scene-damage rect (and clear it). Returns 1 with the box in
 * *x,*y,*w,*h when there is damage, else 0 (nothing changed since the last present). */
int  nw_take_damage(struct nw_server *s, int *x, int *y, int *w, int *h);
/* Run dialog: its on-screen rect (for drawing/damage), and taking the committed command.
 * nw_run_take_spawn returns 1 and copies the command into out[cap] when a launch is pending
 * (Enter was pressed), clearing the request; else 0. */
void nw_run_rect(const struct nw_server *s, int *x, int *y, int *w, int *h);
int  nw_run_take_spawn(struct nw_server *s, char *out, int cap);

/* ---- system authentication ("sudo") dialog — compositor-owned, modal ---- */
/* Open it: show the password prompt to run `cmd` (with optional argv[1] `arg`) as root. */
void nw_auth_begin(struct nw_server *s, const char *cmd, const char *arg);
void nw_auth_rect(const struct nw_server *s, int *x, int *y, int *w, int *h);  /* panel box */
void nw_auth_btn_rect(const struct nw_server *s, int which, int *x, int *y, int *w, int *h); /* 0=OK 1=Cancel */
int  nw_auth_hit(const struct nw_server *s, int px, int py);  /* button under (px,py): 0/1, else -1 */
void nw_auth_key(struct nw_server *s, unsigned char code);    /* type/Backspace/Enter(submit)/Esc(cancel) */
void nw_auth_cancel(struct nw_server *s);                     /* dismiss + scrub the password */
void nw_auth_submit(struct nw_server *s);                     /* arm the elevated launch + close */
/* When a submit is pending, copy out the command/arg/password (each into a cap-sized buffer),
 * scrub the stored password, and return 1; else 0. The shell then runs cmd as root via nwsu. */
int  nw_auth_take(struct nw_server *s, char *cmd, char *arg, char *pass, int cap);

/* ---- global menu (pure helpers, shared by compositing + hit-testing + tests) ---- */
/* Parse a menu spec (0x1e between menus, 0x1f between a menu's title + item labels). */
int  nw_menu_top_count(const char *spec);                       /* number of top menus */
int  nw_menu_top_title(const char *spec, int i, char *out, int cap);  /* -> title length */
int  nw_menu_item_count(const char *spec, int menu);
int  nw_menu_item_label(const char *spec, int menu, int item, char *out, int cap);  /* 1/0 */
/* Screen x-range of the focused app's top menu `i` in the bar (after the logo). */
void nw_menubar_top_x(const struct nw_server *s, int i, int *x, int *w);
/* Which menu a bar click hits: a top index >=0, NW_MENU_LOGO, or NW_MENU_NONE. */
int  nw_menubar_hit(const struct nw_server *s, int px, int py);
/* The open dropdown's on-screen rect + item count; nw_menu_item_at maps a point to an item. */
void nw_menu_dropdown_rect(const struct nw_server *s, int *x, int *y, int *w, int *h);
int  nw_menu_open_item_count(const struct nw_server *s);
int  nw_menu_item_at(const struct nw_server *s, int px, int py);   /* item index or -1 */
/* Label of item `i` in the currently open dropdown (handles the logo menu vs an app menu). */
int  nw_menu_open_label(const struct nw_server *s, int i, char *out, int cap);

/* ---- taskbar (pure helpers, shared by compositing + hit-testing + tests) ---- */
/* The taskbar shows one button per open window in stable slot order. nw_task_count is how many;
 * nw_task_window maps a button index -> window index (or -1). nw_taskbar_button_rect gives a
 * button's on-screen rect; nw_start_rect the Start button; nw_taskbar_hit classifies a point as
 * NW_TB_START / NW_TB_TASK (with *winidx set) / NW_TB_NONE. */
int  nw_task_count(const struct nw_server *s);
int  nw_task_window(const struct nw_server *s, int i);
void nw_start_rect(const struct nw_server *s, int *x, int *y, int *w, int *h);
void nw_taskbar_button_rect(const struct nw_server *s, int i, int *x, int *y, int *w, int *h);
int  nw_taskbar_hit(const struct nw_server *s, int px, int py, int *winidx);

/* ---- exposed pure helpers (also for tests) ---- */
int  nw_hit(const struct nw_server *s, int sx, int sy, int *region);  /* topmost window or -1 */
/* US-layout scancode -> ASCII (0 if not a printable/known key). `shift` applies the shifted
 * glyph. Tab->'\t', Enter->'\n', Backspace->0x08. */
char nw_scancode_ascii(unsigned char code, int shift);

#endif /* NWM_CORE_H */
