#include "doctest.h"
#include "Framebuffer.h"
#include <cstring>

using namespace kernel;

TEST_CASE("fbPutPixel writes the 32-bit word at y*pitch + x*4, honoring pitch padding") {
	const uint32_t W = 4, H = 3, PITCH = W * 4 + 8;   // 8 bytes of row padding
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbSurface s{ buf, PITCH, W, H, 32 };

	fbPutPixel(s, 1, 2, 0x00AABBCC);
	CHECK(*(uint32_t*) (buf + 2 * PITCH + 1 * 4) == 0x00AABBCCu);
	for (uint32_t i = W * 4; i < PITCH; i++)            // padding untouched
		CHECK(buf[i] == 0);
	fbPutPixel(s, 99, 99, 0xFFFFFF);                    // out of bounds: no-op, no crash
}

TEST_CASE("fbFillRect clips to the surface bounds") {
	const uint32_t W = 4, H = 3, PITCH = W * 4;
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbSurface s{ buf, PITCH, W, H, 32 };

	fbFillRect(s, 2, 1, 100, 100, 0x00112233);          // overflows -> clipped
	CHECK(*(uint32_t*) (buf + 1 * PITCH + 2 * 4) == 0x00112233u);
	CHECK(*(uint32_t*) (buf + 2 * PITCH + 3 * 4) == 0x00112233u);
	CHECK(*(uint32_t*) (buf) == 0u);                    // (0,0) outside rect
}

TEST_CASE("fbBlitGlyph: set bits -> fg, clear -> bg, MSB is leftmost") {
	const uint32_t W = 8, H = 16, PITCH = W * 4;
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbSurface s{ buf, PITCH, W, H, 32 };

	unsigned char g[16];
	memset(g, 0, sizeof g);
	g[0] = 0x80;                                         // only top-left pixel set
	fbBlitGlyph(s, g, 0, 0, 0x00FFFFFF, 0x00000001);
	CHECK(*(uint32_t*) (buf + 0 * PITCH + 0 * 4) == 0x00FFFFFFu);  // x=0 -> fg
	CHECK(*(uint32_t*) (buf + 0 * PITCH + 1 * 4) == 0x00000001u);  // x=1 -> bg
}

TEST_CASE("fbScrollUp shifts rows up and clears the exposed bottom") {
	const uint32_t W = 2, H = 4, PITCH = W * 4;
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbSurface s{ buf, PITCH, W, H, 32 };
	for (uint32_t y = 0; y < H; y++)
		fbFillRect(s, 0, y, W, 1, 0x100 + y);           // distinct color per row

	fbScrollUp(s, 1, 0x00DEAD);
	CHECK(*(uint32_t*) (buf + 0 * PITCH) == 0x101u);    // row0 <- old row1
	CHECK(*(uint32_t*) (buf + 2 * PITCH) == 0x103u);    // row2 <- old row3
	CHECK(*(uint32_t*) (buf + 3 * PITCH) == 0x00DEADu); // bottom cleared
}

TEST_CASE("row offset uses 64-bit arithmetic (no 32-bit wrap at large pitch*y)") {
	// pitch chosen so that y*pitch crosses 2^32 for y within height; we only verify the
	// computed byte offset via fbByteOffset (pure, no real allocation of the surface).
	FbSurface s{ (uint8_t*) 0, /*pitch*/ 0x01000000u, /*w*/ 0x003FFFFFu, /*h*/ 0x200u, 32 };
	size_t off = kernel::fbByteOffset(s, 0, 0x101);   // 0x101 * 0x01000000 = 0x1_01000000 (>4 GiB)
	CHECK(off == (size_t) 0x101 * 0x01000000u);
	CHECK((off >> 32) != 0);                          // genuinely past 4 GiB
}

TEST_CASE("fbPutPixel 24bpp writes B,G,R bytes in memory order") {
	const uint32_t W = 2, H = 1, PITCH = W * 3;
	unsigned char buf[PITCH * H];
	memset(buf, 0, sizeof buf);
	FbSurface s{ buf, PITCH, W, H, 24 };

	fbPutPixel(s, 0, 0, 0x00112233);                    // R=0x11 G=0x22 B=0x33
	CHECK(buf[0] == 0x33);
	CHECK(buf[1] == 0x22);
	CHECK(buf[2] == 0x11);
}
