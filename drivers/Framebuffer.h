/*
 * Framebuffer.h — a tiny machine-independent software renderer over a linear
 * framebuffer (the vesafb/simplefb model: the bootloader/firmware owns the mode,
 * we just write pixels). No hardware knowledge — it operates on a caller-supplied
 * base pointer, so it is fully host-testable (render into a malloc'd buffer).
 *
 * Colors are passed as 0x00RRGGBB; the renderer writes the byte order the common
 * 32bpp (BGRX) and 24bpp (BGR) linear framebuffers expect.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct FbSurface {
	uint8_t* base;             // first byte of the framebuffer
	uint32_t pitch;            // bytes per scanline (stride; >= width * bpp/8)
	uint32_t width, height;    // pixels
	uint8_t  bpp;              // 32 or 24
};

void fbPutPixel(const FbSurface& s, uint32_t x, uint32_t y, uint32_t rgb);
void fbFillRect(const FbSurface& s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);

// Rasterize a 16-byte glyph (one byte per scanline, MSB = leftmost pixel) at (x,y),
// painting set bits with `fg` and clear bits with `bg`.
void fbBlitGlyph(const FbSurface& s, const unsigned char* glyph16,
                 uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);

// Scroll the whole surface up by `pixels` rows, clearing the exposed bottom to `bg`.
void fbScrollUp(const FbSurface& s, uint32_t pixels, uint32_t bg);

}  // namespace kernel
