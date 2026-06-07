#include "doctest.h"
#include "KeyDecoder.h"

using namespace kernel;

namespace {
struct Rec {
	int ev[8];
	int n = 0;
	void operator()(int e) { if (n < 8) ev[n++] = e; }
};
}

TEST_CASE("KeyDecoder: letters/digits map to ASCII; Enter/Backspace to control bytes") {
	KeyDecoder d;
	Rec r;
	d.feed(0x1E, r);   // 'a'
	d.feed(0x0B, r);   // '0'
	d.feed(0x1C, r);   // Enter
	d.feed(0x0E, r);   // Backspace
	REQUIRE(r.n == 4);
	CHECK(r.ev[0] == 'a');
	CHECK(r.ev[1] == '0');
	CHECK(r.ev[2] == '\n');
	CHECK(r.ev[3] == 0x08);
}

TEST_CASE("KeyDecoder: key releases are ignored") {
	KeyDecoder d;
	Rec r;
	d.feed(0x1E, r);          // 'a' press
	d.feed(0x1E | 0x80, r);   // 'a' release -> ignored
	CHECK(r.n == 1);
	CHECK(r.ev[0] == 'a');
}

TEST_CASE("KeyDecoder: arrow keys (0xE0 prefix) become KEY_* events") {
	KeyDecoder d;
	Rec r;
	d.feed(0xE0, r); d.feed(0x48, r);   // up
	d.feed(0xE0, r); d.feed(0x50, r);   // down
	d.feed(0xE0, r); d.feed(0x4D, r);   // right
	d.feed(0xE0, r); d.feed(0x4B, r);   // left
	REQUIRE(r.n == 4);
	CHECK(r.ev[0] == KEY_UP);
	CHECK(r.ev[1] == KEY_DOWN);
	CHECK(r.ev[2] == KEY_RIGHT);
	CHECK(r.ev[3] == KEY_LEFT);
}

TEST_CASE("KeyDecoder: extended key release is ignored") {
	KeyDecoder d;
	Rec r;
	d.feed(0xE0, r); d.feed(0x48 | 0x80, r);   // up release
	CHECK(r.n == 0);
}
