#include "doctest.h"
#include "MouseDevice.h"

using namespace kext;

// Read all queued events out of the device.
static int drain(MouseDevice& m, InputEvent* out, int max) {
	int n = 0;
	InputEvent e;
	while (n < max && m.read(0, &e, sizeof e) == (int) sizeof e)
		out[n++] = e;
	return n;
}

TEST_CASE("mouse: move right+up emits REL_X / REL_Y (Y inverted) / SYN, stamped") {
	MouseDevice m;
	m.feed(0x08, 1000000);   // b0: bit3 set, no buttons/signs/overflow
	m.feed(5, 1000000);      // dx = +5
	m.feed(3, 1000000);      // dy = +3 (PS/2 up)
	InputEvent ev[8];
	REQUIRE(drain(m, ev, 8) == 3);
	CHECK(ev[0].type == EV_REL); CHECK(ev[0].code == REL_X); CHECK(ev[0].value == 5);
	CHECK(ev[1].type == EV_REL); CHECK(ev[1].code == REL_Y); CHECK(ev[1].value == -3); // inverted
	CHECK(ev[2].type == EV_SYN); CHECK(ev[2].code == SYN_REPORT);
	CHECK(ev[0].tv_sec == 1); CHECK(ev[0].tv_usec == 0);   // from now_us = 1000000
}

TEST_CASE("mouse: sign bits give negative deltas") {
	MouseDevice m;
	m.feed(0x38, 0);         // b0: bit3 + X-sign(0x10) + Y-sign(0x20)
	m.feed(0xFB, 0);         // dx magnitude -> -5
	m.feed(0xFD, 0);         // dy magnitude -> -3
	InputEvent ev[8];
	REQUIRE(drain(m, ev, 8) == 3);
	CHECK(ev[0].value == -5);          // REL_X
	CHECK(ev[1].value == 3);           // REL_Y = -(-3)
}

TEST_CASE("mouse: button press/release emit EV_KEY edges only") {
	MouseDevice m;
	m.feed(0x09, 0); m.feed(0, 0); m.feed(0, 0);   // left down
	m.feed(0x08, 0); m.feed(0, 0); m.feed(0, 0);   // left up
	InputEvent ev[8];
	REQUIRE(drain(m, ev, 8) == 4);
	CHECK(ev[0].type == EV_KEY); CHECK(ev[0].code == BTN_LEFT); CHECK(ev[0].value == 1);
	CHECK(ev[1].type == EV_SYN);
	CHECK(ev[2].type == EV_KEY); CHECK(ev[2].code == BTN_LEFT); CHECK(ev[2].value == 0);
	CHECK(ev[3].type == EV_SYN);
}

TEST_CASE("mouse: overflow packets dropped; resync on bit3") {
	MouseDevice m;
	m.feed(0x48, 0); m.feed(99, 0); m.feed(99, 0);   // X-overflow (bit6) -> whole packet dropped
	InputEvent ev[8];
	CHECK(drain(m, ev, 8) == 0);
	m.feed(0x00, 0);                                 // invalid first byte (no bit3) -> skipped
	m.feed(0x08, 0); m.feed(1, 0); m.feed(0, 0);     // valid: REL_X +1, SYN
	REQUIRE(drain(m, ev, 8) == 2);
	CHECK(ev[0].type == EV_REL); CHECK(ev[0].value == 1);
	CHECK(ev[1].type == EV_SYN);
}
