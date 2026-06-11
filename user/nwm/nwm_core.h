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
	NW_DOCK_H      = 62,    /* bottom dock height (the rounded translucent pill)            */
	NW_RUN_W       = 460,   /* the Super+R "Run" dialog box */
	NW_RUN_H       = 60,
	NW_RUN_MAX     = 120    /* max command length typed into it */
};

/* Panel button ids (nw_panel_hit). */
enum { NW_PANEL_NONE = 0, NW_PANEL_QUIT = 1, NW_PANEL_SHUTDOWN = 2 };

/* Normalised /dev/input0 scancodes we special-case (bit7=extended, bits0-6=set-1). */
enum {
	NW_SC_LSUPER = 0xDB, NW_SC_RSUPER = 0xDC,   /* 0xE0 0x5B / 0x5C: the GUI (Super/Cmd) keys */
	NW_SC_LSHIFT = 0x2A, NW_SC_RSHIFT = 0x36,
	NW_SC_C = 0x2E, NW_SC_X = 0x2D, NW_SC_V = 0x2F, NW_SC_Q = 0x10, NW_SC_TAB = 0x0F,
	NW_SC_R = 0x13, NW_SC_ESC = 0x01, NW_SC_ENTER = 0x1C, NW_SC_BACKSP = 0x0E
};

/* Hit-test regions. */
enum { NW_HIT_NONE = 0, NW_HIT_CONTENT = 1, NW_HIT_TITLE = 2, NW_HIT_CLOSE = 3 };

struct nw_window {
	int       used;
	uint32_t  id;
	int       client;
	int       x, y;        /* frame top-left, in screen pixels                       */
	int       cw, ch;      /* content size                                           */
	uint32_t *buf;         /* content pixels (cw*ch), bound by the shell after create */
	char      title[NW_TITLE_MAX];
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

	int   super_down, shift_down;

	char  clip[NW_CLIP_MAX];
	int   clip_len;

	struct nw_outq out[NW_MAX_CLIENTS];
	int   client_used[NW_MAX_CLIENTS];
	int   client_dead[NW_MAX_CLIENTS];  /* output overran its ring -> shell disconnects */

	int   want_quit, want_shutdown; /* a panel button was clicked -> the shell acts       */

	/* Super+R "Run" launcher dialog (WM chrome). While open it captures the keyboard. */
	int   run_open;
	char  run_text[NW_RUN_MAX];
	int   run_len;
	char  run_cmd[NW_RUN_MAX];      /* the committed command (on Enter)                  */
	int   want_spawn;               /* shell: launch run_cmd, then clear                 */

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
void nw_pointer(struct nw_server *s, int sx, int sy, int buttons);
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
/* Top-panel buttons. nw_panel_hit returns NW_PANEL_* for a point (0 if not on a button);
 * nw_panel_button_rect gives a button's screen rect (for drawing). Shared so compose and
 * hit-testing agree on geometry. */
int  nw_panel_hit(const struct nw_server *s, int x, int y);
void nw_panel_button_rect(const struct nw_server *s, int id, int *x, int *y, int *w, int *h);
/* Run dialog: its on-screen rect (for drawing/damage), and taking the committed command.
 * nw_run_take_spawn returns 1 and copies the command into out[cap] when a launch is pending
 * (Enter was pressed), clearing the request; else 0. */
void nw_run_rect(const struct nw_server *s, int *x, int *y, int *w, int *h);
int  nw_run_take_spawn(struct nw_server *s, char *out, int cap);

/* ---- exposed pure helpers (also for tests) ---- */
int  nw_hit(const struct nw_server *s, int sx, int sy, int *region);  /* topmost window or -1 */
/* US-layout scancode -> ASCII (0 if not a printable/known key). `shift` applies the shifted
 * glyph. Tab->'\t', Enter->'\n', Backspace->0x08. */
char nw_scancode_ascii(unsigned char code, int shift);

#endif /* NWM_CORE_H */
