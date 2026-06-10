/*
 * libnw.c — NanWM client library (see libnw.h). Talks to the compositor over the inherited
 * pipe pair: fd 3 = requests (client -> server), fd 4 = events (server -> client). Uses the
 * client's libc for I/O; statically linked into each GUI client.
 */
#include "libnw.h"
#include "nwproto.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#define NW_REQ_FD 3
#define NW_EVT_FD 4

struct nw_win {
	nw_display *d;
	uint32_t    id;
	int         w, h;
	uint32_t   *px;
};

struct nw_display {
	int reqfd, evtfd;
	struct nw_decoder dec;
	unsigned char decpay[512];     /* payload assembly for incoming events (e.g. PASTE) */
	unsigned char rbuf[2048];      /* bytes read but not yet decoded                    */
	int rpos, rlen;
};

/* ---- low-level I/O ---- */
static int write_all(int fd, const void *buf, int len)
{
	const unsigned char *p = (const unsigned char *) buf;
	int off = 0;
	while (off < len) {
		int n = (int) write(fd, p + off, len - off);
		if (n <= 0)
			return -1;
		off += n;
	}
	return 0;
}

static int send_hdr(int fd, uint32_t type, uint32_t window, int a, int b, int c, int d, uint32_t len)
{
	struct nw_msg m;
	m.type = type; m.window = window; m.a = a; m.b = b; m.c = c; m.d = d; m.length = len;
	unsigned char hdr[NW_MSG_HDR];
	nw_msg_encode(&m, hdr);
	return write_all(fd, hdr, NW_MSG_HDR);
}

/* ---- connect / create ---- */
nw_display *nw_connect(void)
{
	nw_display *d = (nw_display *) malloc(sizeof *d);
	if (!d)
		return 0;
	d->reqfd = NW_REQ_FD;
	d->evtfd = NW_EVT_FD;
	nw_decoder_init(&d->dec, d->decpay, sizeof d->decpay);
	d->rpos = d->rlen = 0;
	send_hdr(d->reqfd, NW_REQ_HELLO, 0, NW_PROTO_VERSION, 0, 0, 0, 0);
	return d;
}

nw_win *nw_create_window(nw_display *d, int w, int h, const char *title)
{
	if (w <= 0 || h <= 0)
		return 0;
	int tl = title ? (int) strlen(title) : 0;
	if (send_hdr(d->reqfd, NW_REQ_CREATE_WINDOW, 0, w, h, 0, 0, (uint32_t) tl) < 0)
		return 0;
	if (tl && write_all(d->reqfd, title, tl) < 0)
		return 0;

	nw_win *win = (nw_win *) malloc(sizeof *win);
	if (!win)
		return 0;
	win->d = d; win->w = w; win->h = h; win->id = 0;
	win->px = (uint32_t *) malloc((size_t) w * h * 4);
	if (!win->px) { free(win); return 0; }
	memset(win->px, 0, (size_t) w * h * 4);

	/* block for the server's CONFIGURE to learn our window id (startup-only events drop) */
	struct nw_event ev;
	while (nw_next_event(d, &ev, -1) == 1) {
		if (ev.type == NW_EV_CONFIGURE) { win->id = ev.window; break; }
	}
	return win;
}

void nw_win_surface(nw_win *win, struct nw_surface *out)
{
	out->px = win->px; out->w = win->w; out->h = win->h; out->stride = win->w;
	nw_surface_noclip(out);                  /* clients draw to the whole window buffer */
}
int      nw_win_width(nw_win *win)  { return win->w; }
int      nw_win_height(nw_win *win) { return win->h; }
uint32_t nw_win_id(nw_win *win)     { return win->id; }

void nw_commit(nw_win *win, int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > win->w) w = win->w - x;
	if (y + h > win->h) h = win->h - y;
	if (w <= 0 || h <= 0)
		return;
	nw_display *d = win->d;
	if (send_hdr(d->reqfd, NW_REQ_COMMIT, win->id, x, y, w, h, (uint32_t) (w * h * 4)) < 0)
		return;
	for (int r = 0; r < h; r++)
		if (write_all(d->reqfd, win->px + (long) (y + r) * win->w + x, w * 4) < 0)
			return;
}

void nw_set_clipboard(nw_display *d, const char *text, int len)
{
	if (len < 0) len = 0;
	if (send_hdr(d->reqfd, NW_REQ_SET_CLIPBOARD, 0, 0, 0, 0, 0, (uint32_t) len) < 0)
		return;
	if (len) write_all(d->reqfd, text, len);
}

void nw_get_clipboard(nw_display *d)
{
	send_hdr(d->reqfd, NW_REQ_GET_CLIPBOARD, 0, 0, 0, 0, 0, 0);   /* -> arrives as NW_EV_PASTE */
}

/* ---- event pump ---- */
static int translate(nw_display *d, struct nw_event *ev)
{
	const struct nw_msg *m = &d->dec.msg;
	memset(ev, 0, sizeof *ev);
	ev->window = m->window;
	switch (m->type) {
	case NW_EVT_CONFIGURE: ev->type = NW_EV_CONFIGURE; ev->x = m->a; ev->y = m->b; break;
	case NW_EVT_KEY:       ev->type = NW_EV_KEY; ev->ch = (char) m->a; ev->down = m->b; ev->code = m->c; ev->mods = m->d; break;
	case NW_EVT_POINTER:   ev->type = NW_EV_POINTER; ev->x = m->a; ev->y = m->b; ev->buttons = m->c; break;
	case NW_EVT_FOCUS:     ev->type = NW_EV_FOCUS; ev->focus = m->a; break;
	case NW_EVT_CLOSE:     ev->type = NW_EV_CLOSE; break;
	case NW_EVT_COPY:      ev->type = NW_EV_COPY; ev->cut = m->a; break;
	case NW_EVT_PASTE:     ev->type = NW_EV_PASTE; ev->text = (const char *) d->decpay; ev->text_len = (int) m->length; break;
	default:               ev->type = NW_EV_NONE; break;
	}
	return 1;
}

int nw_next_event(nw_display *d, struct nw_event *ev, int timeout_ms)
{
	for (;;) {
		if (d->rpos < d->rlen) {
			const unsigned char *p   = d->rbuf + d->rpos;
			const unsigned char *end = d->rbuf + d->rlen;
			if (nw_decoder_next(&d->dec, &p, end)) {
				d->rpos = (int) (p - d->rbuf);
				return translate(d, ev);
			}
			d->rpos = d->rlen;          /* all bytes folded into the decoder's partial state */
		}
		struct pollfd pfd;
		pfd.fd = d->evtfd; pfd.events = POLLIN; pfd.revents = 0;
		int pr = poll(&pfd, 1, timeout_ms);
		if (pr == 0)
			return 0;                   /* timed out */
		if (pr < 0)
			return -1;
		int n = (int) read(d->evtfd, d->rbuf, sizeof d->rbuf);
		if (n <= 0)
			return -1;                  /* compositor closed the connection */
		d->rpos = 0; d->rlen = n;
	}
}
