/*
 * FbConsole.h — a text console over a linear framebuffer (the Linux fbcon model).
 *
 * Machine-independent: a character grid over an FbSurface, rasterizing glyphs with the
 * 8x16 font and scrolling by copying pixels. Mirrors the VGA text-mode semantics
 * (\b \t \r \n, wrap, scroll) so it can back the same <arch/console.h> sink. The arch
 * layer owns one instance and forwards consolePutChar/Clear/SetCursor to it.
 */
#pragma once
#include "Framebuffer.h"

namespace kernel {

class FbConsole {
	FbSurface m_surf;
	uint32_t m_cols, m_rows;   // grid size in character cells
	uint32_t m_cx, m_cy;       // cursor cell
	uint32_t m_fg, m_bg;
public:
	FbConsole();

	void init(const FbSurface& s);          // grid = w/FONT_W x h/FONT_H, clear to bg
	void clear();
	void putChar(char c);                   // \b \t \r \n, printable glyph, wrap, scroll
	void setCursor(unsigned x, unsigned y); // clamped to the grid

	uint32_t cols() const { return m_cols; }
	uint32_t rows() const { return m_rows; }
	uint32_t cursorX() const { return m_cx; }
	uint32_t cursorY() const { return m_cy; }
};

}  // namespace kernel
