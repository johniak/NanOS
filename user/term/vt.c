/*
 * vt.c — the pure VT engine (see vt.h). Ported from nterm's parser to operate on a vt struct
 * with no globals, no I/O and no rendering. Marks per-row dirty so a renderer repaints minimally.
 */
#include "vt.h"

uint32_t vt_pal(int i)
{
	static const uint32_t base[16] = {
		0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
		0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff };
	if (i < 0) i = 0;
	if (i < 16) return base[i];
	if (i < 232) { i -= 16; int r = i / 36, g = (i / 6) % 6, b = i % 6;
		int R = r ? r * 40 + 55 : 0, G = g ? g * 40 + 55 : 0, B = b ? b * 40 + 55 : 0;
		return (uint32_t) ((R << 16) | (G << 8) | B); }
	int v = (i - 232) * 10 + 8;
	return (uint32_t) ((v << 16) | (v << 8) | v);
}

static int rgb_to_pal(int r, int g, int b)
{
	int lr = (r * 5 + 127) / 255, lg = (g * 5 + 127) / 255, lb = (b * 5 + 127) / 255;
	return 16 + 36 * lr + 6 * lg + lb;
}

static void mark(vt *t, int y) { if (y >= 0 && y < VT_MAXR) t->dirty[y] = 1; }

static void clear_region(vt *t, int x0, int y0, int x1, int y1)
{
	for (int y = y0; y <= y1 && y < t->rows; y++) {
		for (int x = x0; x <= x1 && x < t->cols; x++) {
			t->grid[y][x].ch = ' '; t->grid[y][x].fg = t->fg; t->grid[y][x].bg = t->bg;
		}
		mark(t, y);
	}
}

static void scroll_up(vt *t)
{
	for (int y = t->top; y < t->bot; y++) { for (int x = 0; x < t->cols; x++) t->grid[y][x] = t->grid[y + 1][x]; mark(t, y); }
	for (int x = 0; x < t->cols; x++) { t->grid[t->bot][x].ch = ' '; t->grid[t->bot][x].fg = t->fg; t->grid[t->bot][x].bg = t->bg; }
	mark(t, t->bot);
}

static void put_glyph(vt *t, unsigned char ch)
{
	if (t->cx >= t->cols) { t->cx = 0; if (++t->cy > t->bot) { t->cy = t->bot; scroll_up(t); } }
	if (t->cx < 0 || t->cx >= VT_MAXC || t->cy < 0 || t->cy >= VT_MAXR) return;
	vt_cell *c = &t->grid[t->cy][t->cx];
	c->ch = ch;
	c->fg = t->rev ? t->bg : (t->bold && t->fg < 8 ? t->fg + 8 : t->fg);
	c->bg = t->rev ? t->fg : t->bg;
	mark(t, t->cy);
	t->cx++;
	t->last = ch;
}

static void line_feed(vt *t) { if (++t->cy > t->bot) { t->cy = t->bot; scroll_up(t); } }

/* Switch to / from the alternate screen (DECSET 47/1047/1049). On entry the main screen + cursor
 * are stashed and the screen is cleared (a blank scratch buffer for the full-screen app); on exit
 * the main screen + cursor are restored and every row marked dirty so the renderer repaints it.
 * This is why a normal terminal shows your shell again the moment vim/less/top exit. */
static void swap_screen(vt *t, int enter)
{
	if (enter && !t->alt) {
		t->alt_cx = t->cx; t->alt_cy = t->cy;
		for (int y = 0; y < t->rows; y++)
			for (int x = 0; x < t->cols; x++) t->save[y][x] = t->grid[y][x];
		t->alt = 1;
		clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
	} else if (!enter && t->alt) {
		for (int y = 0; y < t->rows; y++) {
			for (int x = 0; x < t->cols; x++) t->grid[y][x] = t->save[y][x];
			mark(t, y);
		}
		t->cx = t->alt_cx; t->cy = t->alt_cy;
		t->alt = 0;
	}
}

static int clampr(vt *t, int v) { return v < 0 ? 0 : (v >= t->rows ? t->rows - 1 : v); }
static int clampc(vt *t, int v) { return v < 0 ? 0 : (v >= t->cols ? t->cols - 1 : v); }

static void sgr(vt *t)
{
	if (t->npar == 0) { t->par[0] = 0; t->npar = 1; }
	for (int i = 0; i < t->npar; i++) {
		int p = t->par[i];
		if (p == 0) { t->fg = 7; t->bg = 0; t->bold = 0; t->rev = 0; }
		else if (p == 1) t->bold = 1;
		else if (p == 7) t->rev = 1;
		else if (p == 22) t->bold = 0;
		else if (p == 27) t->rev = 0;
		else if (p >= 30 && p <= 37) t->fg = p - 30;
		else if (p == 39) t->fg = 7;
		else if (p >= 40 && p <= 47) t->bg = p - 40;
		else if (p == 49) t->bg = 0;
		else if (p >= 90 && p <= 97) t->fg = 8 + (p - 90);
		else if (p >= 100 && p <= 107) t->bg = 8 + (p - 100);
		else if (p == 38 && i + 2 < t->npar && t->par[i + 1] == 5) { t->fg = t->par[i + 2]; i += 2; }
		else if (p == 48 && i + 2 < t->npar && t->par[i + 1] == 5) { t->bg = t->par[i + 2]; i += 2; }
		else if (p == 38 && i + 4 < t->npar && t->par[i + 1] == 2) { t->fg = rgb_to_pal(t->par[i + 2], t->par[i + 3], t->par[i + 4]); i += 4; }
		else if (p == 48 && i + 4 < t->npar && t->par[i + 1] == 2) { t->bg = rgb_to_pal(t->par[i + 2], t->par[i + 3], t->par[i + 4]); i += 4; }
	}
}

static void blank_cell(vt *t, int y, int x) { t->grid[y][x].ch = ' '; t->grid[y][x].fg = t->fg; t->grid[y][x].bg = t->bg; }

/* ICH (CSI n @): insert n blank cells at the cursor, shifting the rest of the line right; cells
 * pushed past the right margin are lost. */
static void insert_chars(vt *t, int n)
{
	int y = t->cy;
	if (y < 0 || y >= t->rows) return;
	if (n < 1) n = 1;
	if (n > t->cols - t->cx) n = t->cols - t->cx;
	for (int x = t->cols - 1; x >= t->cx + n; x--) t->grid[y][x] = t->grid[y][x - n];
	for (int x = t->cx; x < t->cx + n && x < t->cols; x++) blank_cell(t, y, x);
	mark(t, y);
}

/* DCH (CSI n P): delete n cells at the cursor, shifting the rest of the line left; blanks fill in
 * at the right margin. */
static void delete_chars(vt *t, int n)
{
	int y = t->cy;
	if (y < 0 || y >= t->rows) return;
	if (n < 1) n = 1;
	if (n > t->cols - t->cx) n = t->cols - t->cx;
	for (int x = t->cx; x < t->cols - n; x++) t->grid[y][x] = t->grid[y][x + n];
	for (int x = t->cols - n; x < t->cols; x++) if (x >= 0) blank_cell(t, y, x);
	mark(t, y);
}

/* ECH (CSI n X): erase n cells from the cursor (set to blank); the cursor does not move. */
static void erase_chars(vt *t, int n)
{
	int y = t->cy;
	if (y < 0 || y >= t->rows) return;
	if (n < 1) n = 1;
	for (int x = t->cx; x < t->cx + n && x < t->cols; x++) blank_cell(t, y, x);
	mark(t, y);
}

/* IL (CSI n L): insert n blank lines at the cursor row, scrolling the rows below it down within
 * the scroll region. No-op if the cursor is outside the region. */
static void insert_lines(vt *t, int n)
{
	if (t->cy < t->top || t->cy > t->bot) return;
	if (n < 1) n = 1;
	if (n > t->bot - t->cy + 1) n = t->bot - t->cy + 1;
	for (int y = t->bot; y >= t->cy + n; y--) { for (int x = 0; x < t->cols; x++) t->grid[y][x] = t->grid[y - n][x]; mark(t, y); }
	for (int y = t->cy; y < t->cy + n; y++) { for (int x = 0; x < t->cols; x++) blank_cell(t, y, x); mark(t, y); }
}

/* DL (CSI n M): delete n lines at the cursor row, scrolling the rows below it up within the scroll
 * region; blank lines fill in at the bottom. No-op if the cursor is outside the region. */
static void delete_lines(vt *t, int n)
{
	if (t->cy < t->top || t->cy > t->bot) return;
	if (n < 1) n = 1;
	if (n > t->bot - t->cy + 1) n = t->bot - t->cy + 1;
	for (int y = t->cy; y <= t->bot - n; y++) { for (int x = 0; x < t->cols; x++) t->grid[y][x] = t->grid[y + n][x]; mark(t, y); }
	for (int y = t->bot - n + 1; y <= t->bot; y++) { for (int x = 0; x < t->cols; x++) blank_cell(t, y, x); mark(t, y); }
}

static void csi_final(vt *t, unsigned char f)
{
	int a = t->npar > 0 ? t->par[0] : 0;
	int b = t->npar > 1 ? t->par[1] : 0;
	mark(t, t->cy);
	switch (f) {
	case 'H': case 'f': t->cy = clampr(t, (a ? a : 1) - 1); t->cx = clampc(t, (b ? b : 1) - 1); break;
	case 'A': t->cy = clampr(t, t->cy - (a ? a : 1)); break;
	case 'B': t->cy = clampr(t, t->cy + (a ? a : 1)); break;
	case 'C': t->cx = clampc(t, t->cx + (a ? a : 1)); break;
	case 'D': t->cx = clampc(t, t->cx - (a ? a : 1)); break;
	case 'G': t->cx = clampc(t, (a ? a : 1) - 1); break;
	case 'd': t->cy = clampr(t, (a ? a : 1) - 1); break;
	case 'J':
		if (a == 2 || a == 3) clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
		else if (a == 1) clear_region(t, 0, 0, t->cx, t->cy);
		else { clear_region(t, t->cx, t->cy, t->cols - 1, t->cy);
			if (t->cy + 1 < t->rows) clear_region(t, 0, t->cy + 1, t->cols - 1, t->rows - 1); }
		break;
	case 'K':
		if (a == 2) clear_region(t, 0, t->cy, t->cols - 1, t->cy);
		else if (a == 1) clear_region(t, 0, t->cy, t->cx, t->cy);
		else clear_region(t, t->cx, t->cy, t->cols - 1, t->cy);
		break;
	case 'b':                                /* REP: repeat the last graphic char n times */
		for (int i = 0, n = a ? a : 1; i < n; i++) put_glyph(t, t->last);
		break;
	case '@': insert_chars(t, a); break;     /* ICH */
	case 'P': delete_chars(t, a); break;     /* DCH */
	case 'X': erase_chars(t, a); break;      /* ECH */
	case 'L': insert_lines(t, a); break;     /* IL  */
	case 'M': delete_lines(t, a); break;     /* DL  */
	case 'm': sgr(t); break;
	case 'r': t->top = a ? clampr(t, a - 1) : 0; t->bot = b ? clampr(t, b - 1) : t->rows - 1;
		t->cx = 0; t->cy = t->top; break;
	case 's': t->savecx = t->cx; t->savecy = t->cy; break;
	case 'u': t->cx = t->savecx; t->cy = t->savecy; break;
	case 'h': case 'l':   /* DECSET/DECRST: alternate screen (the rest are noted + ignored) */
		if (t->priv && (a == 1049 || a == 1047 || a == 47))
			swap_screen(t, f == 'h');
		break;
	}
	mark(t, t->cy);
}

enum { S_NORM, S_ESC, S_ESCINT, S_CSI, S_OSC };

static void feed_one(vt *t, unsigned char c)
{
	switch (t->state) {
	case S_NORM:
		if (c == 0x1b) { t->state = S_ESC; return; }
		if (c == '\n') { line_feed(t); t->cx = 0; return; }
		if (c == '\r') { t->cx = 0; return; }
		if (c == '\b') { if (t->cx > 0) t->cx--; return; }
		if (c == '\t') { t->cx = clampc(t, (t->cx + 8) & ~7); return; }
		if (c == 7) return;
		if (c >= 32) put_glyph(t, c);
		return;
	case S_ESC:
		if (c == '[') { t->state = S_CSI; t->npar = 0; t->par[0] = 0; t->priv = 0; return; }
		if (c == ']') { t->state = S_OSC; return; }   // OSC (title/clipboard): swallow to ST/BEL
		if (c == '7') { t->savecx = t->cx; t->savecy = t->cy; }
		else if (c == '8') { t->cx = t->savecx; t->cy = t->savecy; }
		/* ESC + an intermediate byte (0x20-0x2f): '(' ')' '*' '+' charset designators, '#' DECALN,
		 * etc. The intermediate is followed by a *final* byte (the selector, e.g. 'B' in ESC(B that
		 * ncurses emits on every attribute reset) which must be consumed, not printed. */
		else if (c >= 0x20 && c <= 0x2f) { t->state = S_ESCINT; return; }
		/* '=' '>' keypad and other lone final bytes: complete here, nothing trails. */
		t->state = S_NORM;
		return;
	case S_ESCINT:
		if (c >= 0x20 && c <= 0x2f) return;   // further intermediates
		t->state = S_NORM;                    // the final byte (selector) — swallowed
		return;
	case S_CSI:
		/* Private/extension markers ('?' DEC, '<' '=' '>' xterm): note + ignore the sequence's
		 * effect, but keep consuming so its parameters + final byte never leak as text. */
		if (c == '?' || c == '<' || c == '=' || c == '>') { t->priv = 1; return; }
		if (c >= '0' && c <= '9') { if (t->npar == 0) t->npar = 1;
			t->par[t->npar - 1] = t->par[t->npar - 1] * 10 + (c - '0'); return; }
		if (c == ';') { if (t->npar < VT_NPAR) t->par[t->npar++] = 0; return; }
		if (c >= 0x20 && c <= 0x2f) return;   // intermediate bytes (e.g. the space in DECSCUSR)
		csi_final(t, c);                      // (csi_final keys its private-mode actions off t->priv)
		t->state = S_NORM;
		return;
	case S_OSC:
		if (c == 0x07) t->state = S_NORM;            // BEL terminates
		else if (c == 0x1b) t->state = S_ESC;        // ESC '\' (ST): hand the '\' back to S_ESC
		return;
	}
}

void vt_feed(vt *t, const unsigned char *b, int n) { for (int i = 0; i < n; i++) feed_one(t, b[i]); }

void vt_init(vt *t, int cols, int rows)
{
	for (int i = 0; i < (int) sizeof *t; i++) ((unsigned char *) t)[i] = 0;
	t->fg = 7; t->bg = 0;
	vt_resize(t, cols, rows);
	clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
}

void vt_resize(vt *t, int cols, int rows)
{
	if (cols < 1) cols = 1; if (cols > VT_MAXC) cols = VT_MAXC;
	if (rows < 1) rows = 1; if (rows > VT_MAXR) rows = VT_MAXR;
	t->cols = cols; t->rows = rows;
	t->top = 0; t->bot = rows - 1;
	t->cx = clampc(t, t->cx); t->cy = clampr(t, t->cy);
	for (int y = 0; y < rows; y++) mark(t, y);
}
