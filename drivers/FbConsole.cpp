#include "FbConsole.h"
#include "Font8x16.h"

namespace kernel {

FbConsole::FbConsole()
	: m_surf{ 0, 0, 0, 0, 0 }, m_cols(0), m_rows(0), m_cx(0), m_cy(0),
	  m_fg(0x00C0C0C0), m_bg(0x00000000) {}

void FbConsole::init(const FbSurface& s) {
	m_surf = s;
	m_cols = s.width / FONT_W;
	m_rows = s.height / FONT_H;
	// Set colors here, not just in the constructor: the kernel does not run global
	// constructors, so a static FbConsole would otherwise have fg == bg == 0 (black).
	m_fg = 0x00C0C0C0;
	m_bg = 0x00000000;
	clear();
}

void FbConsole::clear() {
	fbFillRect(m_surf, 0, 0, m_surf.width, m_surf.height, m_bg);
	m_cx = 0;
	m_cy = 0;
}

void FbConsole::putChar(char c) {
	if (c == 0x08) {                 // backspace: move left (no erase; the caller redraws)
		if (m_cx > 0)
			m_cx--;
		return;
	} else if (c == 0x09) {          // tab: advance to the next 8-column boundary
		m_cx = (m_cx + 8) & ~7u;
	} else if (c == '\r') {
		m_cx = 0;
	} else if (c == '\n') {
		m_cx = 0;
		m_cy++;
	} else if ((unsigned char) c >= ' ') {
		fbBlitGlyph(m_surf, fontGlyph((unsigned char) c),
				m_cx * FONT_W, m_cy * FONT_H, m_fg, m_bg);
		m_cx++;
	} else {
		return;                      // other control chars: ignore
	}

	if (m_cx >= m_cols) {            // wrap
		m_cx = 0;
		m_cy++;
	}
	if (m_cy >= m_rows) {            // scroll one text row
		fbScrollUp(m_surf, FONT_H, m_bg);
		m_cy = m_rows - 1;
	}
}

void FbConsole::setCursor(unsigned x, unsigned y) {
	m_cx = (m_cols && x >= m_cols) ? m_cols - 1 : x;
	m_cy = (m_rows && y >= m_rows) ? m_rows - 1 : y;
}

}  // namespace kernel
