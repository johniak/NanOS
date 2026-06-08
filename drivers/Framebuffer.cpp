#include "Framebuffer.h"

namespace kernel {

static inline uint8_t* pixelAt(const FbSurface& s, uint32_t x, uint32_t y) {
	return s.base + (uint32_t) y * s.pitch + x * (s.bpp / 8);
}

void fbPutPixel(const FbSurface& s, uint32_t x, uint32_t y, uint32_t rgb) {
	if (x >= s.width || y >= s.height)
		return;
	uint8_t* p = pixelAt(s, x, y);
	if (s.bpp == 32) {
		*(uint32_t*) p = rgb;                  // BGRX in memory (little-endian)
	} else if (s.bpp == 24) {
		p[0] = (uint8_t) (rgb);                // B
		p[1] = (uint8_t) (rgb >> 8);           // G
		p[2] = (uint8_t) (rgb >> 16);          // R
	}
}

void fbFillRect(const FbSurface& s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
	if (x >= s.width || y >= s.height)
		return;
	uint32_t x1 = x + w, y1 = y + h;
	if (x1 > s.width)  x1 = s.width;
	if (y1 > s.height) y1 = s.height;
	// Write each row directly (no per-pixel function call) — fast enough to clear/scroll a
	// 1024x768 console under emulation.
	if (s.bpp == 32) {
		for (uint32_t yy = y; yy < y1; yy++) {
			uint32_t* row = (uint32_t*) (s.base + (uint32_t) yy * s.pitch) + x;
			for (uint32_t xx = x; xx < x1; xx++)
				*row++ = rgb;
		}
	} else {
		for (uint32_t yy = y; yy < y1; yy++)
			for (uint32_t xx = x; xx < x1; xx++)
				fbPutPixel(s, xx, yy, rgb);
	}
}

void fbBlitGlyph(const FbSurface& s, const unsigned char* glyph16,
                 uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
	for (uint32_t row = 0; row < 16; row++) {
		unsigned char bits = glyph16[row];
		for (uint32_t col = 0; col < 8; col++)
			fbPutPixel(s, x + col, y + row, (bits & (0x80u >> col)) ? fg : bg);
	}
}

void fbScrollUp(const FbSurface& s, uint32_t pixels, uint32_t bg) {
	if (pixels == 0)
		return;
	if (pixels >= s.height) {
		fbFillRect(s, 0, 0, s.width, s.height, bg);
		return;
	}
	// Move whole scanlines up by `pixels` rows. Copy by 32-bit words across the full
	// pitch (4-byte aligned for the modes we use) — fast and overlap-safe (dst rows are
	// strictly below their source rows).
	uint32_t* base32 = (uint32_t*) s.base;
	uint32_t wordsPerRow = s.pitch / 4;
	uint32_t moveRows = s.height - pixels;
	for (uint32_t y = 0; y < moveRows; y++) {
		uint32_t* dst = base32 + (uint32_t) y * wordsPerRow;
		uint32_t* src = base32 + (uint32_t) (y + pixels) * wordsPerRow;
		for (uint32_t i = 0; i < wordsPerRow; i++)
			dst[i] = src[i];
	}
	fbFillRect(s, 0, s.height - pixels, s.width, pixels, bg);
}

}  // namespace kernel
