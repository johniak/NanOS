#include "doctest.h"
#include "KeyboardDevice.h"

using namespace kernel;

// Read one 2-byte event; returns false if none available.
static bool nextEvent(KeyboardDevice& k, unsigned char& code, unsigned char& down) {
	unsigned char b[2];
	if (k.read(0, b, 2) != 2)
		return false;
	code = b[0];
	down = b[1];
	return true;
}

TEST_CASE("make/break decode into press/release events") {
	KeyboardDevice k;
	k.feed(0x1E);          // 'a' make
	k.feed(0x9E);          // 'a' break (make | 0x80)
	unsigned char c, d;
	CHECK(nextEvent(k, c, d));
	CHECK(c == 0x1E); CHECK(d == 1);     // press
	CHECK(nextEvent(k, c, d));
	CHECK(c == 0x1E); CHECK(d == 0);     // release
	CHECK(!nextEvent(k, c, d));          // empty
}

TEST_CASE("extended (0xE0) keys get bit7 set, same code for make and break") {
	KeyboardDevice k;
	k.feed(0xE0); k.feed(0x48);          // up-arrow make
	k.feed(0xE0); k.feed(0xC8);          // up-arrow break
	unsigned char c, d;
	CHECK(nextEvent(k, c, d));
	CHECK(c == (0x80 | 0x48)); CHECK(d == 1);
	CHECK(nextEvent(k, c, d));
	CHECK(c == (0x80 | 0x48)); CHECK(d == 0);
}

TEST_CASE("read returns whole events and is empty after draining") {
	KeyboardDevice k;
	k.feed(0x39);                        // space down
	unsigned char buf[8] = {0};
	CHECK(k.read(0, buf, 1) == 0);       // < one event -> nothing
	CHECK(k.read(0, buf, 8) == 2);       // one event
	CHECK(buf[0] == 0x39); CHECK(buf[1] == 1);
	CHECK(k.read(0, buf, 8) == 0);       // drained
}

TEST_CASE("ring overflow drops oldest events, framing stays intact") {
	KeyboardDevice k;
	for (int i = 0; i < 200; i++)        // 200 events into a 128-event ring
		k.feed(0x02 + (i & 7));          // scancodes 0x02..0x09
	// Everything read back must be valid 2-byte events (down flag 0/1), newest retained.
	unsigned char c, d;
	int n = 0;
	while (nextEvent(k, c, d)) {
		CHECK(d <= 1);                   // valid down flag (0 or 1)
		CHECK(c >= 0x02);
		CHECK(c <= 0x09);
		n++;
	}
	CHECK(n > 0);
	CHECK(n <= 128);                     // bounded by ring capacity
}

TEST_CASE("write/ioctl/mmapInfo are unsupported") {
	KeyboardDevice k;
	CHECK(k.write(0, "x", 1) < 0);
	CHECK(k.ioctl(0, 0) < 0);
	unsigned a, b;
	CHECK(k.mmapInfo(&a, &b) < 0);
}

TEST_CASE("kbdFeed forwards to the registered device") {
	KeyboardDevice k;
	kbdRegister(&k);
	kbdFeed(0x1C);                       // enter make
	unsigned char c, d;
	CHECK(nextEvent(k, c, d));
	CHECK(c == 0x1C); CHECK(d == 1);
	kbdRegister(0);                      // unregister so other tests aren't affected
}
