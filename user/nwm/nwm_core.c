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
/* The maximize control (the "□" glyph) sits one control slot left of the close box. */
static void max_box(const struct nw_window *w, int *cx, int *cy)
{
	close_box(w, cx, cy);
	*cx -= NW_CLOSE;
}
/* The minimize control (the "—" glyph) sits two control slots left of the close box. */
static void min_box(const struct nw_window *w, int *cx, int *cy)
{
	close_box(w, cx, cy);
	*cx -= 2 * NW_CLOSE;
}
static int topmost_visible(const struct nw_server *s);   /* front-most non-minimized window, or -1 */

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
		s->win[old].frame_dirty = 1;     /* its title bar redraws dimmed -> re-render the frame */
	}
	if (idx >= 0 && s->win[idx].used) {
		emit_win(s, idx, NW_EVT_FOCUS, 1, 0, 0, 0, 0, 0);
		damage_frame(s, idx);
		s->win[idx].frame_dirty = 1;
	}
	/* The global menu bar shows the focused window's menu (draw_panel reads s->focus), so it
	 * must be repainted whenever focus changes — otherwise it keeps the old app's menu until
	 * something else damages the bar. The taskbar highlights the focused window's button too. */
	damage(s, 0, 0, s->screen_w, NW_PANEL_H);
	damage(s, 0, s->screen_h - NW_TASK_H, s->screen_w, NW_TASK_H);
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
	s->win[idx].minimized = 0;
	if (s->drag_win == idx)
		s->drag_win = -1;
	if (s->resize_win == idx)
		s->resize_win = -1;
	if (s->focus == idx)
		s->focus = topmost_visible(s);          /* skip minimized windows when refocusing */
	damage(s, 0, s->screen_h - NW_TASK_H, s->screen_w, NW_TASK_H);   /* a taskbar button vanished */
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
	s->resize_win = -1;
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
		if (!w->used || w->minimized)              /* a minimized window is not on the desktop */
			continue;
		int fw = frame_w(w), fh = frame_h(w);
		if (sx < w->x || sy < w->y || sx >= w->x + fw || sy >= w->y + fh)
			continue;
		/* inside the frame: classify */
		int cbx, cby, mbx, mby, xbx, xby;
		close_box(w, &cbx, &cby);
		max_box(w, &xbx, &xby);
		min_box(w, &mbx, &mby);
		if (sx >= cbx && sx < cbx + NW_CLOSE && sy >= cby && sy < cby + NW_CLOSE) {
			if (region) *region = NW_HIT_CLOSE;
		} else if (sx >= xbx && sx < xbx + NW_CLOSE && sy >= xby && sy < xby + NW_CLOSE) {
			if (region) *region = NW_HIT_MAX;
		} else if (sx >= mbx && sx < mbx + NW_CLOSE && sy >= mby && sy < mby + NW_CLOSE) {
			if (region) *region = NW_HIT_MIN;
		} else if (sy < w->y + NW_TITLEBAR_H) {
			if (region) *region = NW_HIT_TITLE;
		} else if (!w->maximized && sx >= w->x + fw - NW_RESIZE_GRIP && sy >= w->y + fh - NW_RESIZE_GRIP) {
			if (region) *region = NW_HIT_RESIZE;   /* bottom-right grip (not while maximized) */
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

/* ---- taskbar -------------------------------------------------------------------- */
int nw_task_count(const struct nw_server *s)
{
	int n = 0;
	for (int i = 0; i < NW_MAX_WINDOWS; i++) if (s->win[i].used) n++;
	return n;
}
/* Map a taskbar button index (0-based, slot order) to its window index, or -1. Slot order keeps
 * a window's button in a stable position even as others open/close (Windows-taskbar behaviour). */
int nw_task_window(const struct nw_server *s, int i)
{
	for (int k = 0; k < NW_MAX_WINDOWS; k++)
		if (s->win[k].used && i-- == 0) return k;
	return -1;
}
void nw_start_rect(const struct nw_server *s, int *x, int *y, int *w, int *h)
{
	*x = 0; *y = s->screen_h - NW_TASK_H; *w = NW_START_W; *h = NW_TASK_H;
}
void nw_taskbar_button_rect(const struct nw_server *s, int i, int *x, int *y, int *w, int *h)
{
	*x = NW_START_W + i * NW_TASK_W; *y = s->screen_h - NW_TASK_H; *w = NW_TASK_W; *h = NW_TASK_H;
}
int nw_taskbar_hit(const struct nw_server *s, int px, int py, int *winidx)
{
	if (py < s->screen_h - NW_TASK_H) return NW_TB_NONE;
	if (px < NW_START_W) return NW_TB_START;
	int i = (px - NW_START_W) / NW_TASK_W;
	if (i >= 0 && i < nw_task_count(s)) { if (winidx) *winidx = nw_task_window(s, i); return NW_TB_TASK; }
	return NW_TB_NONE;
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

/* ---- global menu bar -------------------------------------------------------------- */
#define MENU_SEP_REC ((char) 0x1e)   /* between top menus */
#define MENU_SEP_FLD ((char) 0x1f)   /* between a menu's title + item labels */

/* The logo (system) menu is compositor-owned. */
static const char *const LOGO_ITEMS[4] = { "About This Computer", "Run...", "Shut Down", "Quit" };
enum { LOGO_NITEMS = 4 };

static const char *focus_spec(const struct nw_server *s)
{
	return (s->focus >= 0 && s->win[s->focus].used) ? s->win[s->focus].menu : "";
}
static const char *menu_seg(const char *spec, int idx)   /* start of top menu `idx`, or 0 */
{
	const char *p = spec; int cur = 0;
	if (!spec || !spec[0]) return 0;
	while (*p && cur < idx) { if (*p == MENU_SEP_REC) cur++; p++; }
	return cur == idx ? p : 0;
}

int nw_menu_top_count(const char *spec)
{
	if (!spec || !spec[0]) return 0;
	int n = 1;
	for (const char *p = spec; *p; p++) if (*p == MENU_SEP_REC) n++;
	return n;
}
int nw_menu_top_title(const char *spec, int i, char *out, int cap)
{
	out[0] = 0;
	const char *p = menu_seg(spec, i); if (!p) return 0;
	int n = 0;
	while (*p && *p != MENU_SEP_FLD && *p != MENU_SEP_REC && n < cap - 1) out[n++] = *p++;
	out[n] = 0; return n;
}
int nw_menu_item_count(const char *spec, int menu)
{
	const char *p = menu_seg(spec, menu); if (!p) return 0;
	int n = 0;
	while (*p && *p != MENU_SEP_REC) { if (*p == MENU_SEP_FLD) n++; p++; }
	return n;                                  /* fields after the title = item count */
}
int nw_menu_item_label(const char *spec, int menu, int item, char *out, int cap)
{
	out[0] = 0;
	const char *p = menu_seg(spec, menu); if (!p) return 0;
	int field = 0;
	while (*p && *p != MENU_SEP_REC) {          /* advance to field (item+1): 0=title */
		if (*p == MENU_SEP_FLD) { field++; p++; if (field == item + 1) break; continue; }
		p++;
	}
	if (field != item + 1) return 0;
	int n = 0;
	while (*p && *p != MENU_SEP_FLD && *p != MENU_SEP_REC && n < cap - 1) out[n++] = *p++;
	out[n] = 0; return 1;
}

void nw_menubar_top_x(const struct nw_server *s, int i, int *x, int *w)
{
	const char *spec = focus_spec(s);
	int cx = NW_MENU_X0;
	for (int k = 0; k <= i; k++) {
		char t[40]; int tl = nw_menu_top_title(spec, k, t, sizeof t);
		int ww = tl * 8 + 14;
		if (k == i) { *x = cx; *w = ww; return; }
		cx += ww;
	}
	*x = cx; *w = 0;
}
int nw_menubar_hit(const struct nw_server *s, int px, int py)
{
	if (py < 0 || py >= NW_PANEL_H) return NW_MENU_NONE;
	if (px >= 4 && px < 28) return NW_MENU_LOGO;       /* the NanoOS mark */
	int n = nw_menu_top_count(focus_spec(s));
	for (int i = 0; i < n; i++) { int x, w; nw_menubar_top_x(s, i, &x, &w); if (px >= x && px < x + w) return i; }
	return NW_MENU_NONE;
}

int nw_menu_open_item_count(const struct nw_server *s)
{
	if (!s->menu_open) return 0;
	if (s->menu_which == NW_MENU_LOGO) return LOGO_NITEMS;
	return nw_menu_item_count(focus_spec(s), s->menu_which);
}
int nw_menu_open_label(const struct nw_server *s, int i, char *out, int cap)
{
	if (s->menu_which == NW_MENU_LOGO) {
		if (i < 0 || i >= LOGO_NITEMS) { out[0] = 0; return 0; }
		int n = 0; const char *l = LOGO_ITEMS[i];
		while (l[n] && n < cap - 1) { out[n] = l[n]; n++; } out[n] = 0; return 1;
	}
	return nw_menu_item_label(focus_spec(s), s->menu_which, i, out, cap);
}
void nw_menu_dropdown_rect(const struct nw_server *s, int *x, int *y, int *w, int *h)
{
	*w = NW_MENU_DROP_W;
	*h = nw_menu_open_item_count(s) * NW_MENU_ITEM_H;
	if (s->menu_from_start) {                 /* Start menu: bottom-left, opening upward */
		*x = 0;
		*y = s->screen_h - NW_TASK_H - *h - 6;
		return;
	}
	int dx;
	if (s->menu_which == NW_MENU_LOGO) dx = 4;
	else { int ww; nw_menubar_top_x(s, s->menu_which, &dx, &ww); }
	*x = dx; *y = NW_PANEL_H;
}
int nw_menu_item_at(const struct nw_server *s, int px, int py)
{
	int x, y, w, h; nw_menu_dropdown_rect(s, &x, &y, &w, &h);
	if (px < x || px >= x + w || py < y || py >= y + h) return -1;
	int it = (py - y) / NW_MENU_ITEM_H;
	return it < nw_menu_open_item_count(s) ? it : -1;
}

static void damage_menu(struct nw_server *s)        /* repaint the whole bar + a dropdown column */
{
	damage(s, 0, 0, s->screen_w, NW_PANEL_H + (s->menu_open ? NW_MENU_DROP_W : 0));
	if (s->menu_open) { int x, y, w, h; nw_menu_dropdown_rect(s, &x, &y, &w, &h); damage(s, x, y, w + 2, h + 2); }
}
static void menu_open(struct nw_server *s, int which)
{
	s->menu_open = 1; s->menu_which = which; s->menu_hover = -1; s->menu_from_start = 0; damage_menu(s);
}
static void menu_close(struct nw_server *s)
{
	if (!s->menu_open) return;
	int x, y, w, h; nw_menu_dropdown_rect(s, &x, &y, &w, &h);   /* damage the open dropdown... */
	s->menu_open = 0;
	damage(s, x, y, w + 2, h + 8);                             /* ...before clearing the flag */
	damage(s, 0, 0, s->screen_w, NW_PANEL_H);
	damage(s, 0, s->screen_h - NW_TASK_H, s->screen_w, NW_TASK_H);   /* the Start button highlight */
	s->menu_from_start = 0;
}

static void menu_activate(struct nw_server *s, int item)   /* an item was chosen */
{
	if (s->menu_which == NW_MENU_LOGO) {
		if (item == 0) {                            /* About This Computer */
			const char *cmd = "nwabout"; int i = 0;
			for (; cmd[i] && i < NW_RUN_MAX - 1; i++) s->run_cmd[i] = cmd[i];
			s->run_cmd[i] = 0; s->want_spawn = 1;
		} else if (item == 1) {                      /* Run... -> the Super+R launcher dialog */
			s->run_open = 1; s->run_len = 0; damage_run(s);
		} else if (item == 2) s->want_shutdown = 1;  /* Shut Down */
		else if (item == 3) s->want_quit = 1;        /* Quit (leave the desktop) */
	} else if (s->focus >= 0) {
		emit_win(s, s->focus, NW_EVT_MENU, s->menu_which, item, 0, 0, 0, 0);
	}
	menu_close(s);
}

/* Open the Start menu: the same items as the top-bar logo menu, but anchored above the taskbar
 * (menu_from_start tells nw_menu_dropdown_rect to place it bottom-left, opening upward). */
static void menu_open_start(struct nw_server *s)
{
	s->menu_open = 1; s->menu_which = NW_MENU_LOGO; s->menu_hover = -1; s->menu_from_start = 1;
	damage_menu(s);
}

/* Front-most window that is not minimized (the one focus should land on), or -1. */
static int topmost_visible(const struct nw_server *s)
{
	for (int z = s->zn - 1; z >= 0; z--) {
		int idx = s->zorder[z];
		if (s->win[idx].used && !s->win[idx].minimized) return idx;
	}
	return -1;
}

/* Minimize a window: hide it from the scene (its taskbar button stays) and move focus to the
 * next visible window. Restore: unhide, raise to the front and focus it. Both repaint the whole
 * scene since a window appears/disappears, plus the taskbar (its button highlight changes). */
static void minimize_win(struct nw_server *s, int idx)
{
	if (idx < 0 || !s->win[idx].used || s->win[idx].minimized) return;
	s->win[idx].minimized = 1;
	if (s->focus == idx) set_focus(s, topmost_visible(s));
	damage(s, 0, 0, s->screen_w, s->screen_h);
}
static void restore_win(struct nw_server *s, int idx)
{
	if (idx < 0 || !s->win[idx].used) return;
	s->win[idx].minimized = 0;
	z_raise(s, idx);
	set_focus(s, idx);
	s->win[idx].frame_dirty = 1;
	damage(s, 0, 0, s->screen_w, s->screen_h);
}

/* ---- pointer ---------------------------------------------------------------------- */
/* Resize a window's content to (ncw,nch) at (nx,ny): clamp to the minimum, update geometry, mark
 * the cached frame stale, and tell the client its new size via CONFIGURE so it can re-layout and
 * redraw (NetSurf reformats; apps that ignore resize keep their old content top-left). The shell
 * reallocates the backing buffers when it sees cw/ch changed. Damages the old and new frame. */
static void apply_geom(struct nw_server *s, int idx, int nx, int ny, int ncw, int nch)
{
	struct nw_window *w = &s->win[idx];
	if (ncw < NW_MIN_CW) ncw = NW_MIN_CW;
	if (nch < NW_MIN_CH) nch = NW_MIN_CH;
	if (w->x == nx && w->y == ny && w->cw == ncw && w->ch == nch)
		return;
	damage_frame(s, idx);                            /* erase the old frame */
	w->x = nx; w->y = ny; w->cw = ncw; w->ch = nch;
	/* Drop the old backing buffers: their size no longer matches cw/ch. NULL makes commit_rect
	 * (which strides by the NEW cw) skip until the shell reallocates — otherwise a stale commit
	 * would index the smaller old buffer with the larger new stride and overrun it. reconcile_
	 * buffers reallocates at the new size; the client redraws after the CONFIGURE below. */
	w->buf = 0; w->frame = 0;
	w->frame_dirty = 1;
	emit_win(s, idx, NW_EVT_CONFIGURE, ncw, nch, 0, 0, 0, 0);   /* tell the client to re-layout */
	damage_frame(s, idx);                            /* paint the new frame */
	s->dirty = 1;
}

/* Maximize <-> restore. Maximizing fills the work area (between the top menu bar and the bottom
 * taskbar) and saves the previous geometry; toggling again restores it. */
static void toggle_maximize(struct nw_server *s, int idx)
{
	struct nw_window *w = &s->win[idx];
	if (!w->maximized) {
		w->sx = w->x; w->sy = w->y; w->scw = w->cw; w->sch = w->ch;
		int ww = s->screen_w, wh = s->screen_h - NW_PANEL_H - NW_TASK_H;
		w->maximized = 1;
		apply_geom(s, idx, 0, NW_PANEL_H, ww - 2 * NW_BORDER, wh - NW_TITLEBAR_H - NW_BORDER);
	} else {
		w->maximized = 0;
		apply_geom(s, idx, w->sx, w->sy, w->scw, w->sch);
	}
}

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

	if (s->menu_open) {                              /* an open dropdown eats input */
		int oldh = s->menu_hover;
		s->menu_hover = nw_menu_item_at(s, sx, sy);
		if (s->menu_hover != oldh) damage_menu(s);
		int onbar = nw_menubar_hit(s, sx, sy);
		if (onbar != NW_MENU_NONE && onbar != s->menu_which) menu_open(s, onbar);  /* hover-switch */
		if (left_now && !left_was) {
			if (s->menu_hover >= 0)        menu_activate(s, s->menu_hover);
			else if (onbar == NW_MENU_NONE) menu_close(s);
		}
		s->cursor_x = sx; s->cursor_y = sy; s->buttons = buttons;
		return;
	}

	if (s->resize_win >= 0) {                            /* dragging the bottom-right grip */
		if (left_now) {
			struct nw_window *w = &s->win[s->resize_win];
			int nfw = (sx - s->resize_dx) - w->x;        /* new frame size tracks the cursor */
			int nfh = (sy - s->resize_dy) - w->y;
			int ncw = nfw - 2 * NW_BORDER, nch = nfh - NW_TITLEBAR_H - NW_BORDER;
			int dcw = ncw - w->cw; if (dcw < 0) dcw = -dcw;
			int dch = nch - w->ch; if (dch < 0) dch = -dch;
			if (dcw >= 8 || dch >= 8)                    /* ~8px steps: limit realloc/relayout churn */
				apply_geom(s, s->resize_win, w->x, w->y, ncw, nch);
		} else {
			s->resize_win = -1;        /* drop on release */
		}
		s->cursor_x = sx; s->cursor_y = sy; s->buttons = buttons;
		return;
	}

	if (s->drag_win >= 0) {
		if (left_now) {
			damage_frame(s, s->drag_win);            /* erase the old position */
			s->win[s->drag_win].x = sx - s->drag_dx;
			s->win[s->drag_win].y = sy - s->drag_dy;
			damage_frame(s, s->drag_win);            /* paint the new position */
		} else {
			s->drag_win = -1;          /* drop on release */
		}
	} else if (left_now && !left_was && sy < NW_PANEL_H) {   /* press on the menu bar */
		int which = nw_menubar_hit(s, sx, sy);
		if (which != NW_MENU_NONE) menu_open(s, which);     /* logo or an app menu -> dropdown */
		/* bar area: consume, never reaches the windows below */
	} else if (left_now && !left_was && sy >= s->screen_h - NW_TASK_H) {   /* press on the taskbar */
		int wi = -1, tb = nw_taskbar_hit(s, sx, sy, &wi);
		if (tb == NW_TB_START) {
			menu_open_start(s);                              /* Start button -> Start menu */
		} else if (tb == NW_TB_TASK && wi >= 0) {
			/* Windows behaviour: clicking the active app's button minimizes it; clicking any other
			 * (or a minimized) button restores + focuses it. */
			if (wi == s->focus && !s->win[wi].minimized) minimize_win(s, wi);
			else restore_win(s, wi);
		}
	} else if (left_now && !left_was) {    /* press edge on the desktop */
		int region;
		int widx = nw_hit(s, sx, sy, &region);
		if (widx >= 0) {
			if (region == NW_HIT_MIN) {
				minimize_win(s, widx);                       /* the title-bar — control */
			} else {
				z_raise(s, widx);
				set_focus(s, widx);
				if (region == NW_HIT_CLOSE) {
					emit_win(s, widx, NW_EVT_CLOSE, 0, 0, 0, 0, 0, 0);
				} else if (region == NW_HIT_MAX) {
					toggle_maximize(s, widx);                /* □ control: maximize <-> restore */
				} else if (region == NW_HIT_RESIZE) {
					s->resize_win = widx;                    /* grab the bottom-right grip */
					s->resize_dx = sx - (s->win[widx].x + frame_w(&s->win[widx]));
					s->resize_dy = sy - (s->win[widx].y + frame_h(&s->win[widx]));
				} else if (region == NW_HIT_TITLE) {
					s->drag_win = widx;
					s->drag_dx = sx - s->win[widx].x;
					s->drag_dy = sy - s->win[widx].y;
				}
			}
		} else {
			set_focus(s, -1);
		}
	}

	/* deliver pointer to the window under the cursor's content area (never for the bars) */
	int region2;
	int widx2 = (sy < NW_PANEL_H || sy >= s->screen_h - NW_TASK_H) ? -1 : nw_hit(s, sx, sy, &region2);
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
		case NW_SC_M:                           /* Super+M: maximize / restore the focused window */
			if (s->focus >= 0) toggle_maximize(s, s->focus);
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
		w->frame  = 0; w->frame_dirty = 1;   /* shell binds the frame buffer; render it once bound */
		/* The first few windows get a designed spread (the demo desktop layout); beyond that,
		 * new windows cascade from the top-left. */
		static const int LX[4] = { 90, 520, 150, 70 };
		static const int LY[4] = { 400, 110, 70, 150 };
		if (s->zn < 4) { w->x = LX[s->zn]; w->y = LY[s->zn]; }
		else { w->x = 60 + (s->zn * 28) % 300; w->y = 60 + (s->zn * 28) % 220; }
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
		s->win[idx].frame_dirty = 1;        /* content changed -> the cached frame is stale */
		/* damage just the committed rect, in screen coordinates */
		damage(s, s->win[idx].x + NW_BORDER + m->a, s->win[idx].y + NW_TITLEBAR_H + m->b,
		       m->c, m->d);
		break;
	}
	case NW_REQ_SET_MENU: {
		int idx = -1;                              /* the client's window (clients have one) */
		for (int i = 0; i < NW_MAX_WINDOWS; i++)
			if (s->win[i].used && s->win[i].client == client) { idx = i; break; }
		if (idx < 0) break;
		int n = (int) m->length; if (n > NW_MENU_MAX - 1) n = NW_MENU_MAX - 1;
		if (payload && n > 0) memcpy(s->win[idx].menu, payload, n);
		s->win[idx].menu[n > 0 ? n : 0] = 0;
		s->win[idx].menu_len = n > 0 ? n : 0;
		if (idx == s->focus) { damage(s, 0, 0, s->screen_w, NW_PANEL_H); s->dirty = 1; }
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
	case NW_REQ_SPAWN: {
		/* a client asks the compositor to launch a program — route it through the same
		 * pending-spawn slot the Run dialog uses; the I/O shell does the fork+exec. */
		int n = (int) m->length;
		if (n > NW_RUN_MAX - 1) n = NW_RUN_MAX - 1;
		if (payload && n > 0) memcpy(s->run_cmd, payload, n);
		s->run_cmd[n > 0 ? n : 0] = 0;
		if (n > 0) s->want_spawn = 1;
		break;
	}
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
	case 0x01: return 0x1b;                    /* Esc — vim/readline need it (leave insert mode) */
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
