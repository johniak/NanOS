/*
 * libnw.c — nanowm client library (see libnw.h). Talks to the compositor over the inherited
 * pipe pair: fd 3 = requests (client -> server), fd 4 = events (server -> client). Uses the
 * client's libc for I/O; statically linked into each GUI client.
 */
#include "libnw.h"
#include "nwproto.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>

#define NW_REQ_FD 3
#define NW_EVT_FD 4

struct nw_win {
	nw_display *d;
	uint32_t    id;
	int         w, h;
	uint32_t   *px;                /* the surface apps draw into (shm back buffer, or the
	                                * malloc'd fallback buffer when shm is unavailable)     */
	/* shm double buffer: commits carry coordinates only; the compositor maps the same
	 * physical pages. shm_map[0] == 0 -> legacy pipe commits (pixels in the payload). */
	uint64_t    shm_tok[2];
	uint32_t   *shm_map[2];
	unsigned    shm_bytes;         /* per-buffer mapped length (page-rounded w*h*4) */
	int         shm_back;          /* the buffer px aliases (the one being drawn)   */
	int         shm_busy[2];       /* buffer is held by the server (commit sent, no
	                                * NW_EVT_BUFFER_RELEASE yet) — do not draw into it */
};

/* Events libnw consumes internally (BUFFER_RELEASE) can arrive while nw_commit is blocked
 * waiting for a buffer; app-facing events decoded during that wait are parked here and
 * handed out by the next nw_next_event calls, payload copied so a later message can't
 * clobber it. 16 slots ride out a pointer-move flood; overflow drops the newest (pointer
 * streams are coalesced by every client anyway). */
#define NW_PEND_MAX 16
struct nw_pend { struct nw_event ev; char pay[512]; };

struct nw_display {
	int reqfd, evtfd;
	int shmfd;                     /* /dev/nwshm, opened on first window; -1 = unavailable */
	nw_win *winreg[8];             /* windows on this display (BUFFER_RELEASE routing) */
	int nwin;
	struct nw_pend pend[NW_PEND_MAX];
	int npend, pendrd;
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
	d->shmfd = -2;                 /* not tried yet (-1 = tried and unavailable) */
	d->nwin = 0;
	d->npend = d->pendrd = 0;
	nw_decoder_init(&d->dec, d->decpay, sizeof d->decpay);
	d->rpos = d->rlen = 0;
	send_hdr(d->reqfd, NW_REQ_HELLO, 0, NW_PROTO_VERSION, 0, 0, 0, 0);
	return d;
}

/* ---- shared-memory window surfaces --------------------------------------------------------
 * The window's pixels live in two /dev/nwshm buffers (a double buffer): the app draws into the
 * BACK one (win->px), nw_commit presents it by index (coordinates only on the pipe) and swaps.
 * After the swap the new back buffer holds the frame from two commits ago, so the presented
 * damage rect is copied over — cheap, and it keeps the "px always holds the current window
 * content" contract every partial-repaint client (nwui dirty rects, terminal rows) relies on.
 * Any failure below falls back to the legacy pipe commits — same pixels, just slower. */

static int shm_fd(nw_display *d)
{
	if (d->shmfd == -2)
		d->shmfd = open("/dev/nwshm", O_RDWR);
	return d->shmfd;
}

static void shm_release_local(nw_win *win)
{
	for (int i = 0; i < 2; i++) {
		if (win->shm_map[i])
			munmap(win->shm_map[i], win->shm_bytes);
		win->shm_map[i] = 0;
		win->shm_tok[i] = 0;
	}
	win->shm_bytes = 0;
	win->shm_back = 0;
}

static int translate(nw_display *d, struct nw_event *ev);

/* Block until the server releases shm buffer `idx` of this window (NW_EVT_BUFFER_RELEASE) —
 * the wl_buffer.release discipline: never draw into (or copy into) a buffer the compositor
 * may still be reading. App-facing events decoded while waiting are parked in d->pend for
 * the next nw_next_event calls. A bounded wait (2 s) degrades to "assume released" rather
 * than hanging the client if the compositor stalls. */
static void shm_wait_released(nw_win *win, int idx)
{
	nw_display *d = win->d;
	int guard = 200;                          /* 200 * 10 ms = 2 s upper bound */
	while (win->shm_busy[idx] && guard-- > 0) {
		struct nw_event ev;
		int r;
		/* decode anything already buffered first, then poll for more */
		if (d->rpos < d->rlen) {
			const unsigned char *p   = d->rbuf + d->rpos;
			const unsigned char *end = d->rbuf + d->rlen;
			if (nw_decoder_next(&d->dec, &p, end)) {
				d->rpos = (int) (p - d->rbuf);
				r = translate(d, &ev);
				if (r == 1 && ev.type != NW_EV_NONE && d->npend < NW_PEND_MAX) {
					struct nw_pend *slot = &d->pend[d->npend++];
					slot->ev = ev;
					if (ev.text) {            /* payload: copy out of the shared decode buffer */
						int n = ev.text_len < (int) sizeof slot->pay - 1
						        ? ev.text_len : (int) sizeof slot->pay - 1;
						memcpy(slot->pay, ev.text, (size_t) n);
						slot->pay[n] = 0;
						slot->ev.text_len = n;
					}
				}
				continue;
			}
			d->rpos = d->rlen;
		}
		struct pollfd pfd;
		pfd.fd = d->evtfd; pfd.events = POLLIN; pfd.revents = 0;
		int pr = poll(&pfd, 1, 10);
		if (pr < 0)
			break;
		if (pr == 0)
			continue;
		int n = (int) read(d->evtfd, d->rbuf, sizeof d->rbuf);
		if (n <= 0)
			break;                            /* compositor gone: don't hang the exit path */
		d->rpos = 0; d->rlen = n;
	}
	if (win->shm_busy[idx]) {                 /* fell out on timeout/error, not on release */
		static int trace;
		if (trace < 8) { trace++;
		  char msg[] = "libnw: release WAIT TIMEOUT win=? idx=?\n";
		  msg[32] = (char) ('0' + (win->id % 10));
		  msg[38] = (char) ('0' + (idx & 1));
		  write(2, msg, sizeof msg - 1); }
	}
	win->shm_busy[idx] = 0;
}

/* Allocate + map + announce a double buffer for the CURRENT win->w/h. On success win->px points
 * at the back buffer. Returns 0, or -1 with the window left on the fallback path. */
static int shm_setup(nw_win *win, int w, int h)
{
	nw_display *d = win->d;
	int fd = shm_fd(d);
	if (fd < 0)
		return -1;
	unsigned bytes = (unsigned) (((uint64_t) w * h * 4 + 0xFFFu) & ~0xFFFull);
	uint64_t tok[2] = { 0, 0 };
	uint32_t *map[2] = { 0, 0 };
	for (int i = 0; i < 2; i++) {
		struct nwshm_ioc io = { bytes, 0 };
		if (ioctl(fd, NWSHM_IOC_ALLOC, &io) != 0)
			goto fail;
		tok[i] = io.token;
		map[i] = (uint32_t *) mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		                           (int64_t) io.token);
		if (map[i] == (uint32_t *) -1 || !map[i]) { map[i] = 0; goto fail; }
	}
	{
		unsigned char pay[16];
		for (int i = 0; i < 2; i++)
			for (int b = 0; b < 8; b++)
				pay[i * 8 + b] = (unsigned char) (tok[i] >> (b * 8));
		if (send_hdr(d->reqfd, NW_REQ_SHM_SURFACE, win->id, w, h, 0, 0, sizeof pay) < 0)
			goto fail;
		if (write_all(d->reqfd, pay, sizeof pay) < 0)
			goto fail;
	}
	win->shm_tok[0] = tok[0]; win->shm_tok[1] = tok[1];
	win->shm_map[0] = map[0]; win->shm_map[1] = map[1];
	win->shm_bytes = bytes;
	win->shm_back = 0;
	win->shm_busy[0] = win->shm_busy[1] = 0;
	return 0;
fail:
	for (int i = 0; i < 2; i++) {
		if (map[i]) munmap(map[i], bytes);
		if (tok[i]) { struct nwshm_ioc io = { 0, tok[i] }; ioctl(fd, NWSHM_IOC_FREE, &io); }
	}
	return -1;
}

nw_win *nw_create_window_style(nw_display *d, int w, int h, const char *title, uint32_t style)
{
	if (w <= 0 || h <= 0)
		return 0;
	int tl = title ? (int) strlen(title) : 0;
	if (send_hdr(d->reqfd, NW_REQ_CREATE_WINDOW, 0, w, h, (int32_t) style, 0, (uint32_t) tl) < 0)
		return 0;
	if (tl && write_all(d->reqfd, title, tl) < 0)
		return 0;

	nw_win *win = (nw_win *) malloc(sizeof *win);
	if (!win)
		return 0;
	memset(win, 0, sizeof *win);
	win->d = d; win->w = w; win->h = h; win->id = 0;

	/* block for the server's CONFIGURE to learn our window id (startup-only events drop) */
	struct nw_event ev;
	while (nw_next_event(d, &ev, -1) == 1) {
		if (ev.type == NW_EV_CONFIGURE) { win->id = ev.window; break; }
	}

	/* pixels: a shared double buffer when /dev/nwshm is available (commits = coordinates
	 * only), else the legacy malloc'd buffer (commits push pixels through the pipe). One
	 * stderr line either way — a silent fallback here would quietly re-slow every window. */
	if (shm_setup(win, w, h) == 0) {
		win->px = win->shm_map[0];
		{ static const char m[] = "libnw: shm window surface (double buffer)\n";
		  write(2, m, sizeof m - 1); }
	} else {
		{ static const char m[] = "libnw: no shm surface, pipe commits\n";
		  write(2, m, sizeof m - 1); }
		win->px = (uint32_t *) malloc((size_t) w * h * 4);
		if (!win->px) { free(win); return 0; }
		memset(win->px, 0, (size_t) w * h * 4);
	}
	if (d->nwin < (int) (sizeof d->winreg / sizeof d->winreg[0]))
		d->winreg[d->nwin++] = win;           /* BUFFER_RELEASE routing */
	return win;
}

nw_win *nw_create_window(nw_display *d, int w, int h, const char *title)
{
	return nw_create_window_style(d, w, h, title, 0);
}

void nw_win_surface(nw_win *win, struct nw_surface *out)
{
	out->px = win->px; out->w = win->w; out->h = win->h; out->stride = win->w;
	nw_surface_noclip(out);                  /* clients draw to the whole window buffer */
}
int      nw_win_width(nw_win *win)  { return win->w; }
int      nw_win_height(nw_win *win) { return win->h; }
uint32_t nw_win_id(nw_win *win)     { return win->id; }

/* Reallocate the client draw buffer to a new size — called when the compositor resizes the window
 * (an NW_EV_CONFIGURE after creation). The caller then re-fetches nw_win_surface and redraws at the
 * new size. Without this, a client that kept drawing into the old (smaller) buffer after a resize
 * would write out of bounds. The new buffer is zeroed; the client repaints it. */
void nw_win_resize(nw_win *win, int w, int h)
{
	if (!win || w <= 0 || h <= 0 || (w == win->w && h == win->h))
		return;
	if (win->shm_map[0]) {
		/* shm surface: allocate + announce a NEW double buffer at the new size (the server
		 * frees the old tokens when it processes the replacing SHM_SURFACE — the request pipe
		 * is ordered, so in-flight commits against the old pair land first). Only the local
		 * mappings are dropped here. If the new pair can't be allocated, release the surface
		 * (empty SHM_SURFACE) and fall back to a malloc'd buffer + pipe commits. */
		unsigned oldbytes = win->shm_bytes;
		uint32_t *oldmap[2] = { win->shm_map[0], win->shm_map[1] };
		win->shm_map[0] = win->shm_map[1] = 0;   /* shm_setup must not see the old pair */
		win->shm_tok[0] = win->shm_tok[1] = 0;
		win->shm_bytes = 0;
		win->w = w; win->h = h;
		if (shm_setup(win, w, h) == 0) {
			win->px = win->shm_map[0];
		} else {
			send_hdr(win->d->reqfd, NW_REQ_SHM_SURFACE, win->id, w, h, 0, 0, 0);
			win->px = (uint32_t *) malloc((size_t) w * h * 4);
			if (win->px) memset(win->px, 0, (size_t) w * h * 4);
		}
		if (oldmap[0]) munmap(oldmap[0], oldbytes);
		if (oldmap[1]) munmap(oldmap[1], oldbytes);
		return;
	}
	uint32_t *np = (uint32_t *) realloc(win->px, (size_t) w * h * 4);
	if (!np)
		return;                          /* keep the old buffer on OOM rather than dangle */
	win->px = np; win->w = w; win->h = h;
	memset(win->px, 0, (size_t) w * h * 4);
}

void nw_commit(nw_win *win, int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > win->w) w = win->w - x;
	if (y + h > win->h) h = win->h - y;
	if (w <= 0 || h <= 0)
		return;
	nw_display *d = win->d;
	if (win->shm_map[0]) {
		/* shm present: coordinates only (length carries the buffer index), then swap. Before
		 * touching the new back buffer, wait for the server's BUFFER_RELEASE on it (it may
		 * still be reading the PREVIOUS commit out of it) — then bring it up to the current
		 * frame by copying the just-presented damage over (it holds the frame from two
		 * commits ago). With two buffers this paces a free-running client to compose rate. */
		int pres = win->shm_back;
		if (send_hdr(d->reqfd, pres ? NW_REQ_COMMIT_SHM1 : NW_REQ_COMMIT_SHM,
		             win->id, x, y, w, h, 0) < 0)
			return;
		win->shm_busy[pres] = 1;
		win->shm_back = pres ^ 1;
		shm_wait_released(win, win->shm_back);
		win->px = win->shm_map[win->shm_back];
		for (int r = 0; r < h; r++)
			memcpy(win->px + (long) (y + r) * win->w + x,
			       win->shm_map[pres] + (long) (y + r) * win->w + x, (size_t) w * 4);
		return;
	}
	/* Split into horizontal bands so no single message exceeds NW_COMMIT_MAX_BYTES: a full
	 * repaint of a large window otherwise overflows the compositor's reassembly buffer and the
	 * commit is dropped. Each band is its own COMMIT (the server unions their damage rects). */
	int rowbytes = w * 4;
	int band = NW_COMMIT_MAX_BYTES / (rowbytes > 0 ? rowbytes : 1);
	if (band < 1) band = 1;                         /* a single row already exceeds the cap */
	for (int y0 = 0; y0 < h; y0 += band) {
		int bh = h - y0 < band ? h - y0 : band;
		if (send_hdr(d->reqfd, NW_REQ_COMMIT, win->id, x, y + y0, w, bh,
		             (uint32_t) (w * bh * 4)) < 0)
			return;
		for (int r = 0; r < bh; r++)
			if (write_all(d->reqfd, win->px + (long) (y + y0 + r) * win->w + x, w * 4) < 0)
				return;
	}
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

void nw_drag_begin(nw_display *d, const char *text, int len)
{
	if (len < 0) len = 0;
	if (send_hdr(d->reqfd, NW_REQ_DRAG_BEGIN, 0, 0, 0, 0, 0, (uint32_t) len) < 0)
		return;
	if (len) write_all(d->reqfd, text, len);
}

void nw_spawn_arg(nw_display *d, const char *cmd, const char *arg)
{
	int cl = 0; while (cmd && cmd[cl]) cl++;
	int al = 0; while (arg && arg[al]) al++;
	/* payload = "cmd" or "cmd\0arg" (NUL-separated) */
	uint32_t total = (uint32_t) (cl + (al ? 1 + al : 0));
	if (send_hdr(d->reqfd, NW_REQ_SPAWN, 0, 0, 0, 0, 0, total) < 0)
		return;
	if (cl) write_all(d->reqfd, cmd, cl);
	if (al) { char z = 0; write_all(d->reqfd, &z, 1); write_all(d->reqfd, arg, al); }
}

void nw_spawn(nw_display *d, const char *cmd)
{
	int len = 0;
	while (cmd && cmd[len]) len++;
	if (send_hdr(d->reqfd, NW_REQ_SPAWN, 0, 0, 0, 0, 0, (uint32_t) len) < 0)
		return;
	if (len) write_all(d->reqfd, cmd, len);
}

void nw_reload_settings(nw_display *d)
{
	send_hdr(d->reqfd, NW_REQ_RELOAD_SETTINGS, 0, 0, 0, 0, 0, 0);
}

void nw_set_menu(nw_display *d, const char *spec)
{
	int len = 0;
	while (spec && spec[len]) len++;
	if (send_hdr(d->reqfd, NW_REQ_SET_MENU, 0, 0, 0, 0, 0, (uint32_t) len) < 0)
		return;
	if (len) write_all(d->reqfd, spec, len);
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
	case NW_EVT_POINTER:   ev->type = NW_EV_POINTER; ev->x = m->a; ev->y = m->b;
	                       ev->buttons = m->c & 0xff;          /* low byte = mouse-button bitmask    */
	                       ev->mods = (m->c >> 8) & 0xff;      /* high byte = mods (bit0 shift, bit1 cmd) */
	                       ev->wheel = m->d; break;
	case NW_EVT_FOCUS:     ev->type = NW_EV_FOCUS; ev->focus = m->a; break;
	case NW_EVT_CLOSE:     ev->type = NW_EV_CLOSE; break;
	case NW_EVT_COPY:      ev->type = NW_EV_COPY; ev->cut = m->a; break;
	case NW_EVT_PASTE:     ev->type = NW_EV_PASTE; ev->text = (const char *) d->decpay; ev->text_len = (int) m->length; break;
	case NW_EVT_MENU:      ev->type = NW_EV_MENU; ev->menu = m->a; ev->item = m->b; break;
	case NW_EVT_DRAG_MOTION: ev->type = NW_EV_DRAG_MOTION; ev->x = m->a; ev->y = m->b; ev->mods = m->c; break;
	case NW_EVT_DRAG_LEAVE:  ev->type = NW_EV_DRAG_LEAVE; break;
	case NW_EVT_DROP:        ev->type = NW_EV_DROP; ev->x = m->a; ev->y = m->b; ev->mods = m->c;
	                         ev->text = (const char *) d->decpay; ev->text_len = (int) m->length; break;
	case NW_EVT_BUFFER_RELEASE:
		/* consumed here, never surfaced: the server finished reading this shm buffer */
		for (int i = 0; i < d->nwin; i++)
			if (d->winreg[i] && d->winreg[i]->id == m->window) {
				d->winreg[i]->shm_busy[m->a & 1] = 0;
				break;
			}
		{	static int trace;
			if (trace < 8) { trace++;
			  char msg[] = "libnw: release rx win=? idx=?\n";
			  msg[22] = (char) ('0' + (m->window % 10));
			  msg[28] = (char) ('0' + (m->a & 1));
			  write(2, msg, sizeof msg - 1); }
		}
		ev->type = NW_EV_NONE;
		break;
	default:               ev->type = NW_EV_NONE; break;
	}
	return 1;
}

int nw_event_fd(nw_display *d) { return d->evtfd; }

int nw_next_event(nw_display *d, struct nw_event *ev, int timeout_ms)
{
	if (d->pendrd < d->npend) {               /* events parked while a commit waited */
		struct nw_pend *p = &d->pend[d->pendrd++];
		*ev = p->ev;
		if (p->ev.text) ev->text = p->pay;    /* payload lives in the slot, not decpay */
		if (d->pendrd == d->npend) d->pendrd = d->npend = 0;
		return 1;
	}
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
