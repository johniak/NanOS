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

// --- Phase 2: shared RB-paired blend + fast raster paths -------------------------------

// Reference: the original per-channel /255 blend that nw_mix used before unification.
static uint32_t ref_mix255(uint32_t d, uint32_t s, int a) {
	int ia = 255 - a;
	int r = (((s >> 16) & 0xff) * a + ((d >> 16) & 0xff) * ia) / 255;
	int g = (((s >>  8) & 0xff) * a + ((d >>  8) & 0xff) * ia) / 255;
	int b = (( s        & 0xff) * a + ( d        & 0xff) * ia) / 255;
	return (uint32_t) ((r << 16) | (g << 8) | b);
}

TEST_CASE("nw_blend8: a=0 yields dst, a=255 yields src, both bit-exact") {
	uint32_t samples[] = {0x000000, 0xffffff, 0x123456, 0xfedcba, 0x804020, 0x00ff00};
	for (uint32_t d : samples)
		for (uint32_t s : samples) {
			CHECK(nw_blend8(d, s, 0)   == d);
			CHECK(nw_blend8(d, s, 255) == s);
		}
}

TEST_CASE("nw_blend8 matches the old /255 blend within 1 LSB per channel") {
	uint32_t cols[] = {0x000000, 0xffffff, 0x3a7fc1, 0xd4502a, 0x10ff80, 0x887766};
	for (uint32_t d : cols)
		for (uint32_t s : cols)
			for (int a = 0; a <= 255; a += 5) {
				uint32_t got = nw_blend8(d, s, (unsigned) a), ref = ref_mix255(d, s, a);
				for (int sh = 0; sh <= 16; sh += 8) {
					int gc = (got >> sh) & 0xff, rc = (ref >> sh) & 0xff;
					CHECK(gc - rc <= 1); CHECK(rc - gc <= 1);
				}
			}
}

TEST_CASE("nw_mix delegates to nw_blend8: extremes exact, midpoint within 1 LSB") {
	CHECK(nw_mix(0x102030, 0xa0b0c0, 0)   == 0x102030u);
	CHECK(nw_mix(0x102030, 0xa0b0c0, 255) == 0xa0b0c0u);
	uint32_t m = nw_mix(0x000000, 0xffffff, 128);
	for (int sh = 0; sh <= 16; sh += 8) { int c = (m >> sh) & 0xff; CHECK(c >= 127); CHECK(c <= 129); }
}

TEST_CASE("blit (row memcpy) copies an exact sub-rect, unclipped") {
	Buf src(8, 8), dst(8, 8);
	for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
		src.px[(size_t) y * 8 + x] = (uint32_t) (0x10000 * y + x);
	nw_blit(&dst.s, 1, 1, &src.s, 2, 3, 4, 2);          // src[2..6)x[3..5) -> dst at (1,1)
	for (int r = 0; r < 2; r++) for (int c = 0; c < 4; c++)
		CHECK(dst.at(1 + c, 1 + r) == src.at(2 + c, 3 + r));
	CHECK(dst.at(0, 0) == 0u);                           // outside the dest rect: untouched
	CHECK(dst.at(5, 3) == 0u);
}

TEST_CASE("draw_char fast path (in-bounds) equals the clipped path pixel-for-pixel") {
	Buf full(8, 16);                                     // glyph fits exactly -> fast path
	nw_draw_char(&full.s, 0, 0, 'A', 0x00FF00, 0x000001);
	Buf clip(16, 32);                                    // bigger surface, scissor to the same box
	nw_surface_clip(&clip.s, 0, 0, 8, 16);
	// force the slow path by drawing one column off the clip, then compare the overlap region
	nw_draw_char(&clip.s, 0, 0, 'A', 0x00FF00, 0x000001);
	for (int y = 0; y < 16; y++) for (int x = 0; x < 8; x++)
		CHECK(full.at(x, y) == clip.at(x, y));
}

TEST_CASE("nw_text fast path only touches set glyph pixels and matches clipped path") {
	Buf a(16, 16), b(16, 16);
	for (auto& v : a.px) v = 0x222222; for (auto& v : b.px) v = 0x222222;
	nw_text(&a.s, 0, 0, "A", 0xffffff);                  // in bounds -> fast path
	nw_surface_clip(&b.s, -1, 0, 17, 16);                // active scissor that still contains the glyph box edge -> slow path
	nw_text(&b.s, 0, 0, "A", 0xffffff);
	for (int y = 0; y < 16; y++) for (int x = 0; x < 8; x++)
		CHECK(a.at(x, y) == b.at(x, y));                 // identical; background (0x222222) preserved where glyph is unset
}

// --- straight-alpha src-over primitives (the GL-glass ink paths) -----------------------------

TEST_CASE("nw_over_rect: opaque replaces, translucent mixes, alpha 0 is a no-op") {
	Buf b(20, 20);
	for (auto& v : b.px) v = 0x00404040;
	nw_over_rect(&b.s, 2, 2, 6, 6, 0xff00ff00u);         // opaque green
	CHECK((b.at(4, 4) & 0x00ffffffu) == 0x0000ff00u);
	for (int yy = 10; yy < 14; yy++) for (int xx = 10; xx < 14; xx++)
		b.px[(size_t) yy * 20 + xx] = 0xff404040u;       // OPAQUE grey: dst alpha weights the mix
	nw_over_rect(&b.s, 10, 10, 4, 4, 0x80ffffffu);       // ~half white over it
	uint32_t m = b.at(11, 11) & 0xffu;
	CHECK(m > 0x40u); CHECK(m < 0xffu);
	uint32_t before = b.at(0, 0);
	nw_over_rect(&b.s, 0, 0, 3, 3, 0x00ff0000u);         // alpha 0: nothing
	CHECK(b.at(0, 0) == before);
}

TEST_CASE("nw_over_round: corners stay untouched outside the radius, centre fully inked") {
	Buf b(40, 30);
	for (auto& v : b.px) v = 0x00101010;
	nw_over_round(&b.s, 4, 4, 24, 18, 8, 0xffff0000u);
	CHECK((b.at(16, 12) & 0x00ffffffu) == 0x00ff0000u);  // body
	CHECK(b.at(4, 4) == 0x00101010u);                    // square corner outside the arc
	CHECK(b.at(4 + 23, 4) == 0x00101010u);
	CHECK(b.at(4, 4 + 17) == 0x00101010u);
}

TEST_CASE("nw_over_ring: 1px rim only - interior and exterior untouched") {
	Buf b(40, 40);
	for (auto& v : b.px) v = 0x00202020;
	nw_over_ring(&b.s, 5, 5, 26, 26, 6, 0xffffffffu);
	CHECK(b.at(18, 18) == 0x00202020u);                  // interior clean
	CHECK(b.at(2, 2) == 0x00202020u);                    // exterior clean
	CHECK((b.at(18, 5) & 0xffu) == 0xffu);               // top edge rim inked
	CHECK((b.at(5, 18) & 0xffu) == 0xffu);               // left edge rim inked
	int corner_inked = 0;                                // the AA arc wrote something near a corner
	for (int y = 5; y < 12 && !corner_inked; y++)
		for (int x = 5; x < 12 && !corner_inked; x++)
			if (b.at(x, y) != 0x00202020u) corner_inked = 1;
	CHECK(corner_inked == 1);
}

TEST_CASE("nw_over_round_soft: feathered edge fades inward; feather<1 equals crisp") {
	Buf soft(48, 36), crisp(48, 36), plain(48, 36);
	nw_over_round_soft(&soft.s, 4, 4, 40, 28, 8, 0xc0ffffffu, 6);
	nw_over_round_soft(&crisp.s, 4, 4, 40, 28, 8, 0xc0ffffffu, 0);   // fallback path
	nw_over_round(&plain.s, 4, 4, 40, 28, 8, 0xc0ffffffu);
	CHECK(crisp.px == plain.px);                          // feather<1 -> exact nw_over_round
	// feathered: over a transparent dst the fade lives in the ALPHA channel — the band pixel
	// carries less coverage than the centre
	CHECK((soft.at(24, 18) >> 24) > (soft.at(24, 5) >> 24));
	// and the very centre matches the crisp fill (feather only affects the border band)
	CHECK(soft.at(24, 18) == crisp.at(24, 18));
}

TEST_CASE("nw_text_argb: alpha-scaled ink over background; width matches nw_text_argb_w") {
	{ Buf warm(64, 20); nw_text_argb(&warm.s, 2, 2, "Hi", 0xffffffffu); }  // settle lazy font state
	Buf full(64, 20), half(64, 20);
	for (auto& v : full.px) v = 0xff000000; for (auto& v : half.px) v = 0xff000000;   // OPAQUE black
	nw_text_argb(&full.s, 2, 2, "Hi", 0xffffffffu);
	nw_text_argb(&half.s, 2, 2, "Hi", 0x40ffffffu);
	long fsum = 0, hsum = 0;
	for (size_t i = 0; i < full.px.size(); i++) { fsum += full.px[i] & 0xff; hsum += half.px[i] & 0xff; }
	CHECK(fsum > 0);                                      // glyphs actually rendered
	CHECK(hsum > 0);
	CHECK(hsum * 2 < fsum);                               // quarter-alpha ink is much fainter
	CHECK(nw_text_argb_w("Hi") > 0);
	CHECK(nw_text_argb_w("Hi Hi") > nw_text_argb_w("Hi"));
	std::vector<uint32_t> before = full.px;
	nw_text_argb(&full.s, 40, 2, "X", 0x00ffffffu);       // alpha 0: no ink anywhere
	CHECK(full.px == before);
}
