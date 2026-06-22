#include "doctest.h"
#include "nw_backdrop.h"
#include <vector>

TEST_CASE("rect expand/clamp/intersect/union and predicates") {
	CHECK(nw_rect_empty((nw_rect){0,0,0,5}) == 1);
	CHECK(nw_rect_empty((nw_rect){0,0,3,5}) == 0);

	nw_rect e = nw_rect_expand((nw_rect){10,10,20,20}, 4);
	CHECK(e.x == 6); CHECK(e.y == 6); CHECK(e.w == 28); CHECK(e.h == 28);

	// expand at the screen edge then clamp: never negative origin, never past the screen
	nw_rect c = nw_cache_rect((nw_rect){2,2,10,10}, 5, 100, 100);
	CHECK(c.x == 0); CHECK(c.y == 0);
	CHECK(c.x + c.w <= 100); CHECK(c.y + c.h <= 100);

	nw_rect i = nw_rect_intersect((nw_rect){0,0,10,10}, (nw_rect){5,5,10,10});
	CHECK(i.x == 5); CHECK(i.y == 5); CHECK(i.w == 5); CHECK(i.h == 5);
	CHECK(nw_rect_empty(nw_rect_intersect((nw_rect){0,0,5,5}, (nw_rect){10,10,5,5})) == 1);

	nw_rect u = nw_rect_union((nw_rect){0,0,5,5}, (nw_rect){10,10,5,5});
	CHECK(u.x == 0); CHECK(u.y == 0); CHECK(u.w == 15); CHECK(u.h == 15);

	CHECK(nw_rect_intersects((nw_rect){0,0,10,10}, (nw_rect){5,5,2,2}) == 1);
	CHECK(nw_rect_intersects((nw_rect){0,0,10,10}, (nw_rect){10,0,2,2}) == 0); // touching, not overlapping
}

TEST_CASE("downsample: a solid block collapses to that exact color") {
	std::vector<uint32_t> src(8 * 8, 0x00204060u);
	std::vector<uint32_t> dst(2 * 2, 0u);
	nw_downsample_box(src.data(), 8, 8, 8, dst.data(), 4);
	for (int i = 0; i < 4; i++) CHECK(dst[i] == 0x00204060u);
}

TEST_CASE("downsample: a 2x2 block averages its four colors per channel") {
	// one 2x2 block: R values 0,0,255,255 -> avg 127 ; G,B similar pattern
	uint32_t src[4] = { 0x00000000u, 0x00000000u, 0x00ff0000u, 0x00ff0000u };
	uint32_t dst = 0;
	nw_downsample_box(src, 2, 2, 2, &dst, 2);
	CHECK(((dst >> 16) & 0xff) == 127u);   // (0+0+255+255)/4
	CHECK(((dst >> 8)  & 0xff) == 0u);
	CHECK((dst & 0xff) == 0u);
}

TEST_CASE("upsample: a solid lo-res image stays that exact color at any size") {
	std::vector<uint32_t> lo(2 * 2, 0x00336699u);
	std::vector<uint32_t> out(7 * 5, 0u);
	nw_upsample_bilinear(lo.data(), 2, 2, out.data(), 7, 5, 7);
	for (int i = 0; i < 7 * 5; i++) CHECK(out[i] == 0x00336699u);
}

TEST_CASE("upsample: a 1x1 lo-res image fills the whole output with that color") {
	uint32_t lo = 0x00abcdefu;
	std::vector<uint32_t> out(4 * 4, 0u);
	nw_upsample_bilinear(&lo, 1, 1, out.data(), 4, 4, 4);
	for (int i = 0; i < 16; i++) CHECK(out[i] == 0x00abcdefu);
}

TEST_CASE("upsample: a horizontal two-pixel gradient is monotonic across the row") {
	uint32_t lo[2] = { 0x00000000u, 0x00ff0000u };   // black -> red, left to right
	std::vector<uint32_t> out(8, 0u);
	nw_upsample_bilinear(lo, 2, 1, out.data(), 8, 1, 8);
	int prev = -1;
	for (int x = 0; x < 8; x++) {
		int r = (out[x] >> 16) & 0xff;
		CHECK(r >= prev);          // never decreases
		prev = r;
	}
	CHECK(((out[0] >> 16) & 0xff) == 0);     // leftmost samples the first lo pixel
}

TEST_CASE("cache reuse only when rect matches exactly and not dirty") {
	nw_rect a = {10,10,100,80};
	CHECK(nw_backdrop_reusable(a, a, 0) == 1);
	CHECK(nw_backdrop_reusable(a, a, 1) == 0);                 // dirty
	CHECK(nw_backdrop_reusable(a, (nw_rect){11,10,100,80}, 0) == 0); // moved
	CHECK(nw_backdrop_reusable(a, (nw_rect){10,10,101,80}, 0) == 0); // resized
}

TEST_CASE("visible band: uncovered rect returned as-is; fully covered -> empty") {
	nw_rect r = {0,0,100,100};
	CHECK(nw_rect_visible_band(r, (nw_rect){200,200,10,10}).w == 100);    // disjoint
	CHECK(nw_rect_empty(nw_rect_visible_band(r, (nw_rect){-5,-5,110,110})) == 1); // covered
	nw_rect b = nw_rect_visible_band(r, (nw_rect){0,0,100,40});           // top 40 covered
	CHECK(b.y == 40); CHECK(b.h == 60);                                  // bottom slab survives
}
