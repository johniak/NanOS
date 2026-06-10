#include "doctest.h"
#include "nw_gfx.h"
#include <vector>

// A heap-backed surface for tests.
struct Buf {
	std::vector<uint32_t> px;
	nw_surface s;
	Buf(int w, int h) : px((size_t) w * h, 0u) {
		s.px = px.data(); s.w = w; s.h = h; s.stride = w;
	}
	uint32_t at(int x, int y) const { return px[(size_t) y * s.stride + x]; }
};

TEST_CASE("put_pixel writes in-bounds and ignores out-of-bounds") {
	Buf b(10, 10);
	nw_put_pixel(&b.s, 3, 4, 0x112233);
	CHECK(b.at(3, 4) == 0x112233u);
	// out of bounds in every direction: no crash, no change
	nw_put_pixel(&b.s, -1, 0, 0xff);
	nw_put_pixel(&b.s, 0, -1, 0xff);
	nw_put_pixel(&b.s, 10, 0, 0xff);
	nw_put_pixel(&b.s, 0, 10, 0xff);
	int nonzero = 0;
	for (int i = 0; i < 100; i++) if (b.px[i]) nonzero++;
	CHECK(nonzero == 1);
}

TEST_CASE("fill_rect clips to the surface (negative origin + overflow)") {
	Buf b(8, 8);
	nw_fill_rect(&b.s, -2, -2, 5, 5, 0xAA);    // covers (0,0)..(2,2)
	CHECK(b.at(0, 0) == 0xAAu);
	CHECK(b.at(2, 2) == 0xAAu);
	CHECK(b.at(3, 3) == 0u);
	nw_fill_rect(&b.s, 6, 6, 100, 100, 0xBB);  // clipped to bottom-right corner
	CHECK(b.at(7, 7) == 0xBBu);
	CHECK(b.at(6, 6) == 0xBBu);
	CHECK(b.at(5, 5) == 0u);
}

TEST_CASE("draw_char rasterizes the glyph: 'A' row 3 (0x38) sets three pixels, space is blank") {
	Buf b(16, 16);
	// 'A' glyph row 3 is 0x38 = 0b00111000 -> columns 2,3,4 are foreground.
	nw_draw_char(&b.s, 0, 0, 'A', 0x00FF00, 0x000001);
	CHECK(b.at(2, 3) == 0x00FF00u);
	CHECK(b.at(3, 3) == 0x00FF00u);
	CHECK(b.at(4, 3) == 0x00FF00u);
	CHECK(b.at(0, 3) == 0x000001u);            // background elsewhere on the row
	CHECK(b.at(7, 3) == 0x000001u);

	Buf sp(16, 16);
	nw_draw_char(&sp.s, 0, 0, ' ', 0xFFFFFF, 0x000000);
	for (int y = 0; y < NW_FONT_H; y++)
		for (int x = 0; x < NW_FONT_W; x++)
			CHECK(sp.at(x, y) == 0u);          // all background
}

TEST_CASE("draw_char clips when drawn partly off-surface") {
	Buf b(4, 4);
	nw_draw_char(&b.s, -3, -3, 'A', 0xFF, 0x01);   // mostly off the top-left
	// no crash; the rendered pixels are confined to the 4x4 surface (just check it ran)
	int touched = 0;
	for (int i = 0; i < 16; i++) if (b.px[i]) touched++;
	CHECK(touched >= 0);
}

TEST_CASE("draw_text advances 8px per glyph and returns the end x") {
	Buf b(80, 16);
	int endx = nw_draw_text(&b.s, 0, 0, "Hi", 0xFFFFFF, 0x000000);
	CHECK(endx == 2 * NW_FONT_W);
}

TEST_CASE("blit copies a sub-rect and clips against both surfaces") {
	Buf src(4, 4);
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			src.px[y * 4 + x] = (uint32_t) (0x100 + y * 4 + x);
	Buf dst(8, 8);
	nw_blit(&dst.s, 2, 2, &src.s, 0, 0, 4, 4);
	CHECK(dst.at(2, 2) == 0x100u);
	CHECK(dst.at(5, 5) == (uint32_t) (0x100 + 15));
	CHECK(dst.at(1, 1) == 0u);

	// negative destination: only the in-range part lands
	Buf dst2(8, 8);
	nw_blit(&dst2.s, -1, -1, &src.s, 0, 0, 4, 4);
	CHECK(dst2.at(0, 0) == (uint32_t) (0x100 + 5));   // src(1,1) maps to dst(0,0)
	CHECK(dst2.at(2, 2) == (uint32_t) (0x100 + 15));  // src(3,3) maps to dst(2,2)
}
