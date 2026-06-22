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
