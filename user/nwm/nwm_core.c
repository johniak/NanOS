/*
 * nwm_core.c — implementation of the pure compositor core (see nwm_core.h). No I/O.
 */
#include "nwm_core.h"
#include <string.h>

/* ---- frame geometry --------------------------------------------------------------- */
static int frame_w(const struct nw_window *w) { return w->cw + 2 * NW_BORDER; }
static int frame_h(const struct nw_window *w) { return NW_TITLEBAR_H + w->ch + NW_BORDER; }

static void close_box(const struct nw_window *w, int *cx, int *cy)
{
	*cx = w->x + frame_w(w) - NW_BORDER - NW_CLOSE - 2;
	*cy = w->y + (NW_TITLEBAR_H - NW_CLOSE) / 2;
}

/* ---- scene damage ----------------------------------------------------------------- */
static void damage(struct nw_server *s, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	int x1 = x + w, y1 = y + h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x1 > s->screen_w) x1 = s->screen_w;
	if (y1 > s->screen_h) y1 = s->screen_h;
	if (x >= x1 || y >= y1)
		return;
	if (!s->dmg) {
		s->dmg_x0 = x; s->dmg_y0 = y; s->dmg_x1 = x1; s->dmg_y1 = y1; s->dmg = 1;
	} else {
		if (x  < s->dmg_x0) s->dmg_x0 = x;
		if (y  < s->dmg_y0) s->dmg_y0 = y;
		if (x1 > s->dmg_x1) s->dmg_x1 = x1;
		if (y1 > s->dmg_y1) s->dmg_y1 = y1;
	}
	s->dirty = 1;
}
static void damage_frame(struct nw_server *s, int idx)
{
	struct nw_window *w = &s->win[idx];
	damage(s, w->x, w->y, w->cw + 2 * NW_BORDER, NW_TITLEBAR_H + w->ch + NW_BORDER);
}

int nw_peek_damage(const struct nw_server *s, int *x, int *y, int *w, int *h)
{
	if (!s->dmg)
		return 0;
	*x = s->dmg_x0; *y = s->dmg_y0;
	*w = s->dmg_x1 - s->dmg_x0; *h = s->dmg_y1 - s->dmg_y0;
	return 1;
}

int nw_take_damage(struct nw_server *s, int *x, int *y, int *w, int *h)
{
	int got = nw_peek_damage(s, x, y, w, h);
	s->dmg = 0;
	return got;
}

/* ---- output ring ------------------------------------------------------------------ */
static void emit(struct nw_server *s, int client, uint32_t type, uint32_t window,
                 int a, int b, int c, int d, const unsigned char *pay, uint32_t len)
{
	if (client < 0 || client >= NW_MAX_CLIENTS || !s->client_used[client] || s->client_dead[client])
		return;
	struct nw_outq *q = &s->out[client];
	uint32_t need = NW_MSG_HDR + len;
	if (q->cap - q->count < need) {     /* never write a partial message: drop the client */
		s->client_dead[client] = 1;
		return;
	}
	struct nw_msg m;
	m.type = type; m.window = window;
	m.a = a; m.b = b; m.c = c; m.d = d; m.length = len;
	unsigned char hdr[NW_MSG_HDR];
	nw_msg_encode(&m, hdr);
	for (uint32_t i = 0; i < NW_MSG_HDR; i++) { q->buf[q->head] = hdr[i]; q->head = (q->head + 1) % q->cap; q->count++; }
	for (uint32_t i = 0; i < len; i++)      { q->buf[q->head] = pay[i]; q->head = (q->head + 1) % q->cap; q->count++; }
}

static void emit_win(struct nw_server *s, int widx, uint32_t type, int a, int b, int c, int d,
                     const unsigned char *pay, uint32_t len)
{
	if (widx < 0 || !s->win[widx].used)
		return;
	emit(s, s->win[widx].client, type, s->win[widx].id, a, b, c, d, pay, len);
}

/* ---- z-order ---------------------------------------------------------------------- */
static void z_remove(struct nw_server *s, int idx)
{
	int j = 0;
	for (int i = 0; i < s->zn; i++)
		if (s->zorder[i] != idx)
			s->zorder[j++] = s->zorder[i];
	s->zn = j;
}
static void z_add_front(struct nw_server *s, int idx) { s->zorder[s->zn++] = idx; }
static void z_raise(struct nw_server *s, int idx) { z_remove(s, idx); z_add_front(s, idx); }

/* ---- focus ------------------------------------------------------------------------ */
static void set_focus(struct nw_server *s, int idx)
{
	if (s->focus == idx)
		return;
	int old = s->focus;
	s->focus = idx;
	if (old >= 0 && s->win[old].used) {
		emit_win(s, old, NW_EVT_FOCUS, 0, 0, 0, 0, 0, 0);
		damage_frame(s, old);            /* title bar color changes */
	}
	if (idx >= 0 && s->win[idx].used) {
		emit_win(s, idx, NW_EVT_FOCUS, 1, 0, 0, 0, 0, 0);
		damage_frame(s, idx);
	}
}

/* ---- window table ----------------------------------------------------------------- */
static int alloc_window(struct nw_server *s)
{
	for (int i = 0; i < NW_MAX_WINDOWS; i++)
		if (!s->win[i].used)
			return i;
	return -1;
}
static int find_by_id(const struct nw_server *s, uint32_t id)
{
	for (int i = 0; i < NW_MAX_WINDOWS; i++)
		if (s->win[i].used && s->win[i].id == id)
			return i;
	return -1;
}
static void destroy_window(struct nw_server *s, int idx)
{
	damage_frame(s, idx);          /* the area it occupied must be repainted */
	z_remove(s, idx);
	s->win[idx].used = 0;
	s->win[idx].buf  = 0;       /* the shell frees the backing buffer it allocated */
	if (s->drag_win == idx)
		s->drag_win = -1;
	if (s->focus == idx)
		s->focus = (s->zn > 0) ? s->zorder[s->zn - 1] : -1;
}

/* ---- lifecycle -------------------------------------------------------------------- */
void nw_server_init(struct nw_server *s, int screen_w, int screen_h)
{
	memset(s, 0, sizeof *s);
	s->screen_w = screen_w;
	s->screen_h = screen_h;
	s->next_id  = 1;
	s->focus    = -1;
	s->drag_win = -1;
	s->cursor_x = screen_w / 2;
	s->cursor_y = screen_h / 2;
}

void nw_client_connect(struct nw_server *s, int client, unsigned char *outbuf, uint32_t outcap)
{
	if (client < 0 || client >= NW_MAX_CLIENTS)
		return;
	s->client_used[client] = 1;
	s->client_dead[client] = 0;
	s->out[client].buf = outbuf;
	s->out[client].cap = outcap;
	s->out[client].head = 0;
	s->out[client].count = 0;
}

void nw_client_disconnect(struct nw_server *s, int client)
{
	if (client < 0 || client >= NW_MAX_CLIENTS)
		return;
	for (int i = 0; i < NW_MAX_WINDOWS; i++)
		if (s->win[i].used && s->win[i].client == client)
			destroy_window(s, i);
	s->client_used[client] = 0;
	s->client_dead[client] = 0;
	s->out[client].count = 0;
	s->out[client].head = 0;
	s->dirty = 1;
}

/* ---- hit testing ------------------------------------------------------------------ */
int nw_hit(const struct nw_server *s, int sx, int sy, int *region)
{
	for (int z = s->zn - 1; z >= 0; z--) {     /* front to back */
		int idx = s->zorder[z];
		const struct nw_window *w = &s->win[idx];
		if (!w->used)
			continue;
		int fw = frame_w(w), fh = frame_h(w);
		if (sx < w->x || sy < w->y || sx >= w->x + fw || sy >= w->y + fh)
			continue;
		/* inside the frame: classify */
		int cbx, cby;
		close_box(w, &cbx, &cby);
		if (sx >= cbx && sx < cbx + NW_CLOSE && sy >= cby && sy < cby + NW_CLOSE) {
			if (region) *region = NW_HIT_CLOSE;
		} else if (sy < w->y + NW_TITLEBAR_H) {
			if (region) *region = NW_HIT_TITLE;
		} else {
			int cox = w->x + NW_BORDER, coy = w->y + NW_TITLEBAR_H;
			if (sx >= cox && sx < cox + w->cw && sy >= coy && sy < coy + w->ch) {
				if (region) *region = NW_HIT_CONTENT;
			} else {
				if (region) *region = NW_HIT_TITLE;   /* border: treat as frame (draggable) */
			}
		}
		return idx;
	}
	if (region) *region = NW_HIT_NONE;
	return -1;
}

/* ---- top panel (menu bar) --------------------------------------------------------- */
void nw_panel_button_rect(const struct nw_server *s, int id, int *x, int *y, int *w, int *h)
{
	const int GW = 8;                            /* glyph width */
	int qw = 4 * GW + 12;                        /* "Quit"     */
	int sw = 8 * GW + 12;                        /* "Shutdown" */
	int qx = s->screen_w - qw - 6;
	int sxb = qx - sw - 6;
	*y = 2; *h = NW_PANEL_H - 4;
	if (id == NW_PANEL_SHUTDOWN) { *x = sxb; *w = sw; }
	else                         { *x = qx;  *w = qw; }   /* default: quit */
}

int nw_panel_hit(const struct nw_server *s, int x, int y)
{
	if (y < 0 || y >= NW_PANEL_H)
		return NW_PANEL_NONE;
	int bx, by, bw, bh;
	nw_panel_button_rect(s, NW_PANEL_QUIT, &bx, &by, &bw, &bh);
	if (x >= bx && x < bx + bw && y >= by && y < by + bh)
		return NW_PANEL_QUIT;
	nw_panel_button_rect(s, NW_PANEL_SHUTDOWN, &bx, &by, &bw, &bh);
	if (x >= bx && x < bx + bw && y >= by && y < by + bh)
		return NW_PANEL_SHUTDOWN;
	return NW_PANEL_NONE;
}

/* ---- Run dialog (Super+R launcher) ------------------------------------------------ */
void nw_run_rect(const struct nw_server *s, int *x, int *y, int *w, int *h)
{
	*w = NW_RUN_W; *h = NW_RUN_H;
	*x = (s->screen_w - NW_RUN_W) / 2;
	*y = 140;
}
static void damage_run(struct nw_server *s)
{
	int x, y, w, h;
	nw_run_rect(s, &x, &y, &w, &h);
	damage(s, x - 2, y - 2, w + 4, h + 4);
}
static void run_dialog_key(struct nw_server *s, unsigned char code)
{
	if (code == NW_SC_ESC) { s->run_open = 0; damage_run(s); return; }
	if (code == NW_SC_ENTER) {
		if (s->run_len > 0) {
			memcpy(s->run_cmd, s->run_text, s->run_len);
			s->run_cmd[s->run_len] = 0;
			s->want_spawn = 1;
		}
		s->run_open = 0;
		damage_run(s);
		return;
	}
	if (code == NW_SC_BACKSP) { if (s->run_len > 0) s->run_len--; damage_run(s); return; }
	char ch = nw_scancode_ascii(code, s->shift_down);
	if (ch >= 32 && ch < 127 && s->run_len < NW_RUN_MAX - 1) {
		s->run_text[s->run_len++] = ch;
		damage_run(s);
	}
}

int nw_run_take_spawn(struct nw_server *s, char *out, int cap)
{
	if (!s->want_spawn)
		return 0;
	int i = 0;
	for (; s->run_cmd[i] && i < cap - 1; i++)
		out[i] = s->run_cmd[i];
	out[i] = 0;
	s->want_spawn = 0;
	return 1;
}

/* ---- pointer ---------------------------------------------------------------------- */
void nw_pointer(struct nw_server *s, int sx, int sy, int buttons)
{
	if (sx < 0) sx = 0;
	if (sy < 0) sy = 0;
	if (sx >= s->screen_w) sx = s->screen_w - 1;
	if (sy >= s->screen_h) sy = s->screen_h - 1;
	/* NOTE: cursor motion does NOT dirty the scene — the cursor is a cheap overlay the shell
	 * repaints every frame, so plain mouse movement never repaints windows. */

	int left_now = buttons & NW_BTN_LEFT;
	int left_was = s->buttons & NW_BTN_LEFT;

	if (s->drag_win >= 0) {
		if (left_now) {
			damage_frame(s, s->drag_win);            /* erase the old position */
			s->win[s->drag_win].x = sx - s->drag_dx;
			s->win[s->drag_win].y = sy - s->drag_dy;
			damage_frame(s, s->drag_win);            /* paint the new position */
		} else {
			s->drag_win = -1;          /* drop on release */
		}
	} else if (left_now && !left_was && sy < NW_PANEL_H) {   /* press on the top panel */
		int b = nw_panel_hit(s, sx, sy);
		if (b == NW_PANEL_QUIT)          s->want_quit = 1;
		else if (b == NW_PANEL_SHUTDOWN) s->want_shutdown = 1;
		/* panel area: consume, never reaches the windows below */
	} else if (left_now && !left_was) {    /* press edge on the desktop */
		int region;
		int widx = nw_hit(s, sx, sy, &region);
		if (widx >= 0) {
			z_raise(s, widx);
			set_focus(s, widx);
			if (region == NW_HIT_CLOSE) {
				emit_win(s, widx, NW_EVT_CLOSE, 0, 0, 0, 0, 0, 0);
			} else if (region == NW_HIT_TITLE) {
				s->drag_win = widx;
				s->drag_dx = sx - s->win[widx].x;
				s->drag_dy = sy - s->win[widx].y;
			}
		} else {
			set_focus(s, -1);
		}
	}

	/* deliver pointer to the window under the cursor's content area */
	int region2;
	int widx2 = nw_hit(s, sx, sy, &region2);
	if (widx2 >= 0 && region2 == NW_HIT_CONTENT) {
		int rx = sx - (s->win[widx2].x + NW_BORDER);
		int ry = sy - (s->win[widx2].y + NW_TITLEBAR_H);
		emit_win(s, widx2, NW_EVT_POINTER, rx, ry, buttons, 0, 0, 0);
	}

	s->cursor_x = sx;
	s->cursor_y = sy;
	s->buttons  = buttons;
}

/* ---- keyboard --------------------------------------------------------------------- */
void nw_key(struct nw_server *s, unsigned char code, int down)
{
	if (code == NW_SC_LSUPER || code == NW_SC_RSUPER) { s->super_down = down; return; }
	if (code == NW_SC_LSHIFT || code == NW_SC_RSHIFT) { s->shift_down = down; return; }

	/* Super+R toggles the Run launcher (like Win+R). */
	if (down && s->super_down && code == NW_SC_R) {
		s->run_open = !s->run_open;
		if (s->run_open) s->run_len = 0;
		damage_run(s);
		return;
	}
	/* While the Run dialog is open it captures the keyboard. */
	if (s->run_open) {
		if (down) run_dialog_key(s, code);
		return;
	}

	if (down && s->super_down) {            /* macOS-style Super (Cmd) shortcuts */
		switch (code) {
		case NW_SC_Q:
			if (s->focus >= 0) emit_win(s, s->focus, NW_EVT_CLOSE, 0, 0, 0, 0, 0, 0);
			return;
		case NW_SC_TAB:
			if (s->zn >= 2) { int back = s->zorder[0]; z_raise(s, back); set_focus(s, back); }
			return;
		case NW_SC_C:
			if (s->focus >= 0) emit_win(s, s->focus, NW_EVT_COPY, 0, 0, 0, 0, 0, 0);  /* a=0 copy */
			return;
		case NW_SC_X:
			if (s->focus >= 0) emit_win(s, s->focus, NW_EVT_COPY, 1, 0, 0, 0, 0, 0);  /* a=1 cut */
			return;
		case NW_SC_V:
			if (s->focus >= 0)
				emit_win(s, s->focus, NW_EVT_PASTE, 0, 0, 0, 0,
				         (const unsigned char *) s->clip, (uint32_t) s->clip_len);
			return;
		default:
			return;                         /* other Super+key: consumed, no-op */
		}
	}

	if (s->focus >= 0) {
		char ascii = nw_scancode_ascii(code, s->shift_down);
		emit_win(s, s->focus, NW_EVT_KEY, (unsigned char) ascii, down, code,
		         s->shift_down ? 1 : 0, 0, 0);     /* d = mods (bit0 = shift) */
	}
}

/* ---- client requests -------------------------------------------------------------- */
static void commit_rect(struct nw_window *w, int dx, int dy, int dw, int dh,
                        const unsigned char *payload, uint32_t len)
{
	if (!w->buf || dw <= 0 || dh <= 0)
		return;
	if ((uint32_t) (dw * dh * 4) > len)     /* payload too short for the claimed rect */
		return;
	const uint32_t *src = (const uint32_t *) payload;
	for (int r = 0; r < dh; r++) {
		int y = dy + r;
		if (y < 0 || y >= w->ch)
			continue;
		for (int c = 0; c < dw; c++) {
			int x = dx + c;
			if (x < 0 || x >= w->cw)
				continue;
			w->buf[(long) y * w->cw + x] = src[(long) r * dw + c];
		}
	}
}

void nw_client_msg(struct nw_server *s, int client, const struct nw_msg *m,
                   const unsigned char *payload)
{
	switch (m->type) {
	case NW_REQ_HELLO:
		break;
	case NW_REQ_CREATE_WINDOW: {
		int idx = alloc_window(s);
		if (idx < 0)
			break;
		struct nw_window *w = &s->win[idx];
		memset(w, 0, sizeof *w);
		w->used   = 1;
		w->id     = s->next_id++;
		w->client = client;
		w->cw     = m->a > 0 ? m->a : 1;
		w->ch     = m->b > 0 ? m->b : 1;
		w->buf    = 0;
		w->x      = 40 + (s->zn * 24) % 240;     /* cascade new windows */
		w->y      = 40 + (s->zn * 24) % 160;
		uint32_t tl = m->length < NW_TITLE_MAX - 1 ? m->length : NW_TITLE_MAX - 1;
		if (payload && tl) memcpy(w->title, payload, tl);
		w->title[tl] = 0;
		z_add_front(s, idx);
		set_focus(s, idx);
		emit_win(s, idx, NW_EVT_CONFIGURE, w->cw, w->ch, 0, 0, 0, 0);
		s->dirty = 1;
		break;
	}
	case NW_REQ_COMMIT: {
		int idx = find_by_id(s, m->window);
		if (idx < 0 || s->win[idx].client != client)
			break;
		commit_rect(&s->win[idx], m->a, m->b, m->c, m->d, payload, m->length);
		/* damage just the committed rect, in screen coordinates */
		damage(s, s->win[idx].x + NW_BORDER + m->a, s->win[idx].y + NW_TITLEBAR_H + m->b,
		       m->c, m->d);
		break;
	}
	case NW_REQ_DESTROY_WINDOW: {
		int idx = find_by_id(s, m->window);
		if (idx < 0 || s->win[idx].client != client)
			break;
		destroy_window(s, idx);
		s->dirty = 1;
		break;
	}
	case NW_REQ_SET_CLIPBOARD: {
		int n = (int) m->length;
		if (n > NW_CLIP_MAX) n = NW_CLIP_MAX;
		if (payload && n > 0) memcpy(s->clip, payload, n);
		s->clip_len = n;
		break;
	}
	case NW_REQ_GET_CLIPBOARD:
		/* deliver the clipboard to the requester's focused window (a menu "Paste") */
		if (s->focus >= 0 && s->win[s->focus].client == client)
			emit_win(s, s->focus, NW_EVT_PASTE, 0, 0, 0, 0,
			         (const unsigned char *) s->clip, (uint32_t) s->clip_len);
		break;
	default:
		break;
	}
}

/* ---- shell helpers ---------------------------------------------------------------- */
struct nw_window *nw_window_needs_buffer(struct nw_server *s)
{
	for (int i = 0; i < NW_MAX_WINDOWS; i++)
		if (s->win[i].used && !s->win[i].buf)
			return &s->win[i];
	return 0;
}

const unsigned char *nw_outq_peek(struct nw_server *s, int client, uint32_t *len)
{
	struct nw_outq *q = &s->out[client];
	if (q->count == 0) { *len = 0; return 0; }
	uint32_t tail = (q->head - q->count + q->cap) % q->cap;
	uint32_t span = q->cap - tail;            /* contiguous bytes from tail to buffer end */
	if (span > q->count) span = q->count;
	*len = span;
	return q->buf + tail;
}

void nw_outq_ack(struct nw_server *s, int client, uint32_t n)
{
	struct nw_outq *q = &s->out[client];
	if (n > q->count) n = q->count;
	q->count -= n;
}

uint32_t nw_outq_pending(const struct nw_server *s, int client)
{
	return s->out[client].count;
}

int nw_client_is_dead(const struct nw_server *s, int client)
{
	return s->client_dead[client];
}

/* ---- US scancode -> ASCII --------------------------------------------------------- */
char nw_scancode_ascii(unsigned char code, int shift)
{
	if (code & 0x80)
		return 0;                              /* extended keys have no ASCII here */
	switch (code) {
	case 0x02: return shift ? '!' : '1';
	case 0x03: return shift ? '@' : '2';
	case 0x04: return shift ? '#' : '3';
	case 0x05: return shift ? '$' : '4';
	case 0x06: return shift ? '%' : '5';
	case 0x07: return shift ? '^' : '6';
	case 0x08: return shift ? '&' : '7';
	case 0x09: return shift ? '*' : '8';
	case 0x0A: return shift ? '(' : '9';
	case 0x0B: return shift ? ')' : '0';
	case 0x0C: return shift ? '_' : '-';
	case 0x0D: return shift ? '+' : '=';
	case 0x0E: return 8;                        /* backspace */
	case 0x0F: return '\t';
	case 0x10: return shift ? 'Q' : 'q';
	case 0x11: return shift ? 'W' : 'w';
	case 0x12: return shift ? 'E' : 'e';
	case 0x13: return shift ? 'R' : 'r';
	case 0x14: return shift ? 'T' : 't';
	case 0x15: return shift ? 'Y' : 'y';
	case 0x16: return shift ? 'U' : 'u';
	case 0x17: return shift ? 'I' : 'i';
	case 0x18: return shift ? 'O' : 'o';
	case 0x19: return shift ? 'P' : 'p';
	case 0x1A: return shift ? '{' : '[';
	case 0x1B: return shift ? '}' : ']';
	case 0x1C: return '\n';                     /* Enter */
	case 0x1E: return shift ? 'A' : 'a';
	case 0x1F: return shift ? 'S' : 's';
	case 0x20: return shift ? 'D' : 'd';
	case 0x21: return shift ? 'F' : 'f';
	case 0x22: return shift ? 'G' : 'g';
	case 0x23: return shift ? 'H' : 'h';
	case 0x24: return shift ? 'J' : 'j';
	case 0x25: return shift ? 'K' : 'k';
	case 0x26: return shift ? 'L' : 'l';
	case 0x27: return shift ? ':' : ';';
	case 0x28: return shift ? '"' : '\'';
	case 0x29: return shift ? '~' : '`';
	case 0x2B: return shift ? '|' : '\\';
	case 0x2C: return shift ? 'Z' : 'z';
	case 0x2D: return shift ? 'X' : 'x';
	case 0x2E: return shift ? 'C' : 'c';
	case 0x2F: return shift ? 'V' : 'v';
	case 0x30: return shift ? 'B' : 'b';
	case 0x31: return shift ? 'N' : 'n';
	case 0x32: return shift ? 'M' : 'm';
	case 0x33: return shift ? '<' : ',';
	case 0x34: return shift ? '>' : '.';
	case 0x35: return shift ? '?' : '/';
	case 0x39: return ' ';
	default:   return 0;
	}
}
