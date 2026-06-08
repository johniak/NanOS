/*
 * Font8x16.h — a fixed 8x16 bitmap console font (machine-independent).
 *
 * Each glyph is 16 bytes, one byte per scanline, MSB = leftmost pixel. Used by the
 * framebuffer console to rasterize text. Source data: the public-domain IBM VGA BIOS
 * 8x16 font (see Font8x16.cpp).
 */
#pragma once

namespace kernel {

enum { FONT_W = 8, FONT_H = 16 };

// The 16-byte glyph bitmap for character `c` (0..255).
const unsigned char* fontGlyph(unsigned char c);

}  // namespace kernel
