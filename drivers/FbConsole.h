/*
 * FbConsole.h — a text console over a linear framebuffer (the Linux fbcon model).
 *
 * Machine-independent: it drives the shared VT/ANSI engine (vt.c, the same one nterm/nwterm
 * use) and rasterizes the resulting character grid with the 8x16 font. Because the full xterm
 * escape subset (cursor positioning, erase, scroll region, SGR 16/256, alt-screen) is handled
 * by vt.c, full-screen TUIs (vim, less, top) render correctly on the bare kernel console — not
 * just plain text + a few colors. The arch layer owns one instance and forwards
 * consolePutChar/Clear/SetCursor to it.
 */
#pragma once
#include "Framebuffer.h"
#include "vt.h"   // the shared VT/ANSI terminal engine (grid + escape parser)

namespace kernel {

class FbConsole {
	FbSurface m_surf;
	vt m_vt;                    // terminal grid + escape parser (cols/rows/cursor/dirty live here)
	vt_cell m_shadow[VT_MAXR][VT_MAXC];   // last-blitted cells: only changed cells are repainted
	uint32_t m_fg, m_bg;        // default colours (for the cleared background)
	uint32_t m_curx, m_cury;    // cell where the underline cursor is currently drawn
	bool m_curShown;
	bool m_live = true;         // when false, putChar updates the grid but does not blit

	void renderRow(int row);    // blit one grid row's cells
	void renderDirty();         // blit every row vt marked dirty, clearing the flags
	void drawCursor();          // paint the underline cursor at the vt cursor cell
	void eraseCursor();         // clear the previously-drawn underline
public:
	FbConsole();

	void init(const FbSurface& s);          // grid = w/FONT_W x h/FONT_H, clear to bg
	void clear();
	void putChar(char c);                   // feeds the VT engine (\n -> \r\n), then repaints
	void setCursor(unsigned x, unsigned y); // clamped to the grid

	// Off-screen support for virtual terminals: an inactive console keeps its grid but does not
	// blit. setLive(false) suppresses pixel writes; repaintAll() makes it visible again, blitting
	// the entire grid (the LFB was showing some other VT, so the shadow is stale).
	void setLive(bool on) { m_live = on; }
	bool live() const { return m_live; }
	void repaintAll();

	uint32_t cols() const { return (uint32_t) m_vt.cols; }
	uint32_t rows() const { return (uint32_t) m_vt.rows; }
	uint32_t cursorX() const { return (uint32_t) m_vt.cx; }
	uint32_t cursorY() const { return (uint32_t) m_vt.cy; }
	// Current foreground as an RGB value (bold brightens indices 0..7), as a cell would render.
	uint32_t fg() const { return vt_pal(m_vt.bold && m_vt.fg < 8 ? m_vt.fg + 8 : m_vt.fg); }
};

}  // namespace kernel
