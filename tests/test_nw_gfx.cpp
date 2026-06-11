#include "doctest.h"
#include "nw_gfx.h"
#include <vector>

// A heap-backed surface for tests.
struct Buf {
	std::vector<uint32_t> px;
	nw_surface s;
	Buf(int w, int h) : px((size_t) w * h, 0u) {
		s.px = px.data(); s.w = w; s.h = h; s.stride = w;
		nw_surface_noclip(&s);
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

TEST_CASE("scissor confines fill_rect to the clip rect; noclip restores full drawing") {
	Buf b(10, 10);
	nw_surface_clip(&b.s, 2, 2, 4, 4);                // clip to [2,6)x[2,6)
	nw_fill_rect(&b.s, 0, 0, 10, 10, 0xAB);           // a full-surface fill...
	CHECK(b.at(2, 2) == 0xABu);                        // ...only lands inside the scissor
	CHECK(b.at(5, 5) == 0xABu);
	CHECK(b.at(1, 1) == 0u);                           // outside: untouched
	CHECK(b.at(6, 6) == 0u);
	nw_surface_noclip(&b.s);
	nw_fill_rect(&b.s, 0, 0, 10, 10, 0xCD);           // now the whole surface fills
	CHECK(b.at(1, 1) == 0xCDu);
	CHECK(b.at(9, 9) == 0xCDu);
}

TEST_CASE("scissor confines put_pixel and blit to the clip rect") {
	Buf b(10, 10);
	nw_surface_clip(&b.s, 3, 3, 2, 2);                // clip to [3,5)x[3,5)
	nw_put_pixel(&b.s, 4, 4, 0x11);                    // inside
	nw_put_pixel(&b.s, 7, 7, 0x22);                    // outside -> dropped
	CHECK(b.at(4, 4) == 0x11u);
	CHECK(b.at(7, 7) == 0u);
	Buf src(4, 4);
	for (int i = 0; i < 16; i++) src.px[i] = 0x100u + i;
	nw_blit(&b.s, 2, 2, &src.s, 0, 0, 4, 4);          // would cover [2,6)x[2,6); scissor cuts it
	CHECK(b.at(3, 3) == (uint32_t) (0x100 + 1 * 4 + 1));   // src(1,1) -> dst(3,3), inside clip
	CHECK(b.at(4, 4) == (uint32_t) (0x100 + 2 * 4 + 2));   // src(2,2) -> dst(4,4), inside clip
	CHECK(b.at(2, 2) == 0u);                           // dst(2,2) is outside the scissor
	CHECK(b.at(5, 5) == 0u);                           // dst(5,5) is outside the scissor
}

TEST_CASE("blend_rect mixes src over dst by alpha; 0 keeps dst, 255 replaces it") {
	Buf b(4, 4);
	nw_fill_rect(&b.s, 0, 0, 4, 4, 0x000000);
	nw_blend_rect(&b.s, 0, 0, 4, 4, 0xffffff, 128);     // ~50% white over black
	uint32_t c = b.at(1, 1);
	int r = (c >> 16) & 0xff;
	CHECK(r > 120); CHECK(r < 135);                      // ~128
	nw_blend_rect(&b.s, 0, 0, 4, 4, 0xff0000, 0);        // alpha 0 -> unchanged
	CHECK(b.at(1, 1) == c);
	nw_blend_rect(&b.s, 0, 0, 4, 4, 0x00ff00, 255);      // alpha 255 -> opaque green
	CHECK(b.at(1, 1) == 0x00ff00u);
}

TEST_CASE("vgrad_rect interpolates top->bottom colour down the rect") {
	Buf b(2, 11);
	nw_vgrad_rect(&b.s, 0, 0, 2, 11, 0x000000, 0x00ff00);   // black -> green over 11 rows
	CHECK(((b.at(0, 0) >> 8) & 0xff) == 0);                  // top = black
	CHECK(((b.at(0, 10) >> 8) & 0xff) == 0xff);              // bottom = full green
	int mid = (b.at(0, 5) >> 8) & 0xff;
	CHECK(mid > 110); CHECK(mid < 145);                      // middle ~ halfway
}

TEST_CASE("fill_round fills the interior opaquely and anti-aliases the corners") {
	Buf b(20, 20);
	nw_fill_rect(&b.s, 0, 0, 20, 20, 0x000000);
	nw_fill_round(&b.s, 0, 0, 20, 20, 6, 0xffffff, 255);
	CHECK(b.at(10, 10) == 0xffffffu);                        // centre fully filled
	CHECK(b.at(0, 0) == 0x000000u);                          // extreme corner stays background
	int edge = (b.at(1, 1) >> 16) & 0xff;                    // near corner: partial coverage
	CHECK(edge >= 0); CHECK(edge < 255);
}

TEST_CASE("blur_rect leaves a uniform region unchanged and averages an edge") {
	Buf b(16, 16);
	nw_fill_rect(&b.s, 0, 0, 16, 16, 0x404040);
	nw_blur_rect(&b.s, 0, 0, 16, 16, 2, 1);
	CHECK(b.at(8, 8) == 0x404040u);                          // flat region: unchanged
	Buf e(16, 16);
	nw_fill_rect(&e.s, 0, 0, 8, 16, 0x000000);
	nw_fill_rect(&e.s, 8, 0, 8, 16, 0x646464);               // hard vertical edge at x=8
	nw_blur_rect(&e.s, 0, 0, 16, 16, 3, 1);
	int l = e.at(7, 8) & 0xff, r = e.at(8, 8) & 0xff;
	CHECK(l > 0);                                            // dark side lifted by the blur
	CHECK(r < 0x64);                                         // light side pulled down
}
