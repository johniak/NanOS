/*
 * libnw.h — the NanWM client library: connect to the compositor (over the inherited pipe
 * pair on fds 3/4), create a window, draw into its pixel buffer, commit damage, and pump
 * the event queue. This is the client's GetMessage/DispatchMessage loop. Shipped as the
 * shared library libnw.ndl (the user32/gdi32 of NanWM); clients import it by name.
 */
#ifndef LIBNW_H
#define LIBNW_H

#include <stdint.h>
#include "nw_gfx.h"

typedef struct nw_display nw_display;
typedef struct nw_win nw_win;

/* Event kinds delivered by nw_next_event. */
enum {
	NW_EV_NONE = 0,
	NW_EV_CONFIGURE,   /* w,h            */
	NW_EV_KEY,         /* code,down,ch   */
	NW_EV_POINTER,     /* x,y,buttons    */
	NW_EV_FOCUS,       /* focus (1/0)    */
	NW_EV_CLOSE,
	NW_EV_COPY,        /* cut: a==1      */
	NW_EV_PASTE        /* text,text_len  */
};

struct nw_event {
	int       type;
	uint32_t  window;
	int       x, y, buttons;   /* POINTER */
	int       code, down;      /* KEY: raw scancode + press/release */
	char      ch;              /* KEY: decoded ASCII (0 if none)    */
	int       mods;            /* KEY: modifier bitmask (bit0 = shift) */
	int       focus;           /* FOCUS */
	int       cut;             /* COPY: 1 = cut, 0 = copy */
	const char *text;          /* PASTE: clipboard text (valid until next nw_next_event) */
	int       text_len;
};

/* Connect to the compositor (fds 3/4 set up by the server before exec). NULL on failure. */
nw_display *nw_connect(void);

/* Create a window; blocks for the server's CONFIGURE. Returns NULL on failure. */
nw_win *nw_create_window(nw_display *d, int w, int h, const char *title);

/* The client-side pixel buffer to draw into (w*h, stride = w) and its geometry. */
void      nw_win_surface(nw_win *win, struct nw_surface *out);
int       nw_win_width(nw_win *win);
int       nw_win_height(nw_win *win);
uint32_t  nw_win_id(nw_win *win);

/* Push the damaged rectangle's pixels to the compositor. */
void nw_commit(nw_win *win, int x, int y, int w, int h);

/* Offer text as the clipboard contents (reply to an NW_EV_COPY). */
void nw_set_clipboard(nw_display *d, const char *text, int len);

/* Ask the compositor for the clipboard; it replies with an NW_EV_PASTE event. */
void nw_get_clipboard(nw_display *d);

/* Ask the compositor to launch a program (by name or absolute path), the same path the Run
 * dialog uses. Fire-and-forget; the new program connects as its own client. */
void nw_spawn(nw_display *d, const char *cmd);

/* Wait up to timeout_ms (<0 = forever, 0 = poll) for one event.
 *   1  = an event was written to *ev
 *   0  = timed out, no event
 *  -1  = the compositor closed the connection */
int nw_next_event(nw_display *d, struct nw_event *ev, int timeout_ms);

#endif /* LIBNW_H */
