#include "FbConsole.h"
#include "Font8x16.h"

namespace kernel {

static const uint32_t kDefaultFg = 0x00C0C0C0;

// Standard 16-colour ANSI/VGA palette (0..7 normal, 0..7 bold/bright).
static const uint32_t kAnsiNormal[8] = {
	0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
};
static const uint32_t kAnsiBright[8] = {
	0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
};

FbConsole::FbConsole()
	: m_surf{ 0, 0, 0, 0, 0 }, m_cols(0), m_rows(0), m_cx(0), m_cy(0),
	  m_fg(kDefaultFg), m_bg(0x00000000), m_esc(0), m_npar(0), m_bold(false) {}

void FbConsole::init(const FbSurface& s) {
	m_surf = s;
	m_cols = s.width / FONT_W;
	m_rows = s.height / FONT_H;
	// Set state here, not just in the constructor: the kernel does not run global
	// constructors, so a static FbConsole would otherwise have fg == bg == 0 (black).
	m_fg = kDefaultFg;
	m_bg = 0x00000000;
	m_esc = 0;
	m_npar = 0;
	m_bold = false;
	clear();
}

// Apply one CSI 'm' (SGR) sequence from the accumulated parameters: reset, bold, and
// the 30-37 / 90-97 foreground colors (enough for colored boot status text).
void FbConsole::applySgr() {
	for (int i = 0; i <= m_npar; i++) {
		int n = m_par[i];
		if (n == 0) {            // reset
			m_bold = false;
			m_fg = kDefaultFg;
		} else if (n == 1) {     // bold/bright
			m_bold = true;
		} else if (n >= 30 && n <= 37) {
			m_fg = (m_bold ? kAnsiBright : kAnsiNormal)[n - 30];
		} else if (n >= 90 && n <= 97) {
			m_fg = kAnsiBright[n - 90];
		}
	}
}

void FbConsole::handleEscape(char c) {
	if (m_esc == 1) {                        // just saw ESC
		if (c == '[') {
			m_esc = 2;
			m_npar = 0;
			m_par[0] = 0;
		} else {
			m_esc = 0;                       // unsupported escape: drop it
		}
		return;
	}
	// m_esc == 2: inside a CSI sequence.
	if (c >= '0' && c <= '9') {
		m_par[m_npar] = m_par[m_npar] * 10 + (c - '0');
	} else if (c == ';') {
		if (m_npar < 3)
			m_par[++m_npar] = 0;
	} else {
		if (c == 'm')
			applySgr();
		m_esc = 0;                           // any final byte ends the sequence
	}
}

void FbConsole::clear() {
	fbFillRect(m_surf, 0, 0, m_surf.width, m_surf.height, m_bg);
	m_cx = 0;
	m_cy = 0;
}

void FbConsole::putChar(char c) {
	if (m_esc) {                     // consuming an escape sequence (not rendered)
		handleEscape(c);
		return;
	}
	if (c == 0x1B) {                 // ESC: start of a sequence
		m_esc = 1;
		return;
	}
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
