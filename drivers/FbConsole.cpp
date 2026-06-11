#include "FbConsole.h"
#include "Font8x16.h"

namespace kernel {

static const uint32_t kDefaultFg = 0x00C0C0C0;

FbConsole::FbConsole()
	: m_surf{ 0, 0, 0, 0, 0 }, m_fg(kDefaultFg), m_bg(0x00000000),
	  m_curx(0), m_cury(0), m_curShown(false) {
	m_vt.cols = m_vt.rows = 0;
}

// The cursor is a 2px underline at the bottom of the current cell (no blink — there is no
// console timer). vt's logical cursor can sit one past the last column (deferred wrap); clamp
// it for display so the underline stays on-screen.
void FbConsole::drawCursor() {
	uint32_t cx = (uint32_t) m_vt.cx, cy = (uint32_t) m_vt.cy;
	if (m_vt.cols && cx >= (uint32_t) m_vt.cols) cx = m_vt.cols - 1;
	if (m_vt.rows && cy >= (uint32_t) m_vt.rows) cy = m_vt.rows - 1;
	fbFillRect(m_surf, cx * FONT_W, cy * FONT_H + (FONT_H - 2), FONT_W, 2, vt_pal(m_vt.fg));
	m_curx = cx; m_cury = cy; m_curShown = true;
}
void FbConsole::eraseCursor() {
	if (!m_curShown)
		return;
	fbFillRect(m_surf, m_curx * FONT_W, m_cury * FONT_H + (FONT_H - 2), FONT_W, 2, m_bg);
	m_curShown = false;
}

void FbConsole::renderRow(int row) {
	if (row < 0 || row >= m_vt.rows)
		return;
	// Blit only cells that differ from what is already on screen (the shadow). Typing one
	// character marks the whole row dirty but changes a single cell, so this keeps repaint
	// cost O(changed cells), not O(row width) — essential for a usable console.
	for (int x = 0; x < m_vt.cols; x++) {
		const vt_cell& cell = m_vt.grid[row][x];
		vt_cell& shad = m_shadow[row][x];
		if (cell.ch == shad.ch && cell.fg == shad.fg && cell.bg == shad.bg)
			continue;
		fbBlitGlyph(m_surf, fontGlyph(cell.ch ? cell.ch : ' '),
				x * FONT_W, row * FONT_H, vt_pal(cell.fg), vt_pal(cell.bg));
		shad = cell;
	}
}

void FbConsole::renderDirty() {
	for (int y = 0; y < m_vt.rows; y++) {
		if (m_vt.dirty[y]) {
			renderRow(y);
			m_vt.dirty[y] = 0;
		}
	}
}

void FbConsole::init(const FbSurface& s) {
	m_surf = s;
	m_fg = kDefaultFg;
	m_bg = 0x00000000;
	int cols = (int) (s.width / FONT_W);
	int rows = (int) (s.height / FONT_H);
	if (cols > VT_MAXC) cols = VT_MAXC;
	if (rows > VT_MAXR) rows = VT_MAXR;
	vt_init(&m_vt, cols, rows);
	clear();
}

void FbConsole::clear() {
	fbFillRect(m_surf, 0, 0, m_surf.width, m_surf.height, m_bg);
	// Home + clear the VT grid so its model matches the wiped surface.
	const unsigned char seq[] = { 0x1b, '[', '2', 'J', 0x1b, '[', 'H' };
	vt_feed(&m_vt, seq, sizeof seq);
	// The surface is now blank; sync the shadow to the cleared grid so the diff renderer treats
	// already-blank cells as up to date and only paints text written afterwards.
	for (int y = 0; y < VT_MAXR; y++) {
		for (int x = 0; x < VT_MAXC; x++)
			m_shadow[y][x] = m_vt.grid[y][x];
		m_vt.dirty[y] = 0;
	}
	m_curShown = false;
	drawCursor();
}

void FbConsole::putChar(char c) {
	eraseCursor();
	// vt handles \r \n (as CRLF) \b \t BEL + the escape grammar itself.
	vt_feed(&m_vt, (const unsigned char*) &c, 1);
	renderDirty();
	drawCursor();
}

void FbConsole::setCursor(unsigned x, unsigned y) {
	eraseCursor();
	m_vt.cx = (m_vt.cols && (int) x >= m_vt.cols) ? m_vt.cols - 1 : (int) x;
	m_vt.cy = (m_vt.rows && (int) y >= m_vt.rows) ? m_vt.rows - 1 : (int) y;
	drawCursor();
}

}  // namespace kernel
