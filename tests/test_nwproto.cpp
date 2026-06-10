#include "doctest.h"
#include "nwproto.h"
#include <vector>
#include <cstring>

// Serialize a whole message (header + payload) to a byte vector, the way a peer would
// write it onto the pipe.
static std::vector<unsigned char> wire(const nw_msg& m, const unsigned char* pay) {
	std::vector<unsigned char> v(NW_MSG_HDR);
	nw_msg_encode(&m, v.data());
	for (uint32_t i = 0; i < m.length; i++)
		v.push_back(pay ? pay[i] : (unsigned char) i);
	return v;
}

// Drive the decoder over `bytes`, delivered in chunks of `chunk` bytes (chunk==0 => all at
// once). Collects each completed message as (header, payload-copy).
struct Got { nw_msg m; std::vector<unsigned char> pay; bool overflow; };
static std::vector<Got> run(const std::vector<unsigned char>& bytes, size_t chunk,
                            uint32_t paycap = 4096) {
	std::vector<unsigned char> paybuf(paycap);
	nw_decoder d;
	nw_decoder_init(&d, paybuf.data(), paycap);
	std::vector<Got> out;
	size_t off = 0;
	size_t step = chunk ? chunk : bytes.size();
	if (step == 0) step = 1;
	while (off < bytes.size()) {
		size_t n = bytes.size() - off; if (n > step) n = step;
		const unsigned char* p   = bytes.data() + off;
		const unsigned char* end = p + n;
		while (nw_decoder_next(&d, &p, end)) {
			Got g; g.m = d.msg; g.overflow = d.overflow != 0;
			uint32_t keep = d.msg.length < paycap ? d.msg.length : paycap;
			g.pay.assign(d.payload, d.payload + keep);
			out.push_back(g);
		}
		off += n;
	}
	return out;
}

TEST_CASE("encode writes exactly the 28-byte header verbatim") {
	nw_msg m{}; m.type = NW_REQ_CREATE_WINDOW; m.window = 7; m.a = 320; m.b = 240; m.length = 0;
	unsigned char buf[NW_MSG_HDR];
	CHECK(nw_msg_encode(&m, buf) == NW_MSG_HDR);
	nw_msg back{}; memcpy(&back, buf, NW_MSG_HDR);
	CHECK(back.type == NW_REQ_CREATE_WINDOW);
	CHECK(back.window == 7);
	CHECK(back.a == 320);
	CHECK(back.b == 240);
}

TEST_CASE("decode round-trips a header-only message") {
	nw_msg m{}; m.type = NW_EVT_KEY; m.window = 3; m.a = 0x41; m.b = 1;
	auto bytes = wire(m, nullptr);
	auto got = run(bytes, 0);
	REQUIRE(got.size() == 1);
	CHECK(got[0].m.type == NW_EVT_KEY);
	CHECK(got[0].m.window == 3);
	CHECK(got[0].m.a == 0x41);
	CHECK(got[0].m.b == 1);
	CHECK(got[0].pay.empty());
}

TEST_CASE("decode reassembles a payload message split ONE BYTE at a time") {
	unsigned char pay[100];
	for (int i = 0; i < 100; i++) pay[i] = (unsigned char) (200 - i);
	nw_msg m{}; m.type = NW_REQ_COMMIT; m.window = 1; m.c = 10; m.d = 10; m.length = 100;
	auto bytes = wire(m, pay);
	auto got = run(bytes, 1);                 // feed byte-by-byte: stresses both phases
	REQUIRE(got.size() == 1);
	CHECK(got[0].m.length == 100);
	REQUIRE(got[0].pay.size() == 100);
	CHECK(memcmp(got[0].pay.data(), pay, 100) == 0);
}

TEST_CASE("decode handles a header split mid-way across two reads") {
	nw_msg m{}; m.type = NW_EVT_POINTER; m.window = 2; m.a = 11; m.b = 22; m.c = NW_BTN_LEFT;
	auto bytes = wire(m, nullptr);
	for (size_t cut = 1; cut < NW_MSG_HDR; cut++) {   // split inside the header at every offset
		auto got = run(bytes, cut);
		REQUIRE(got.size() == 1);
		CHECK(got[0].m.a == 11);
		CHECK(got[0].m.c == NW_BTN_LEFT);
	}
}

TEST_CASE("decode separates two messages packed into one read") {
	nw_msg m1{}; m1.type = NW_EVT_FOCUS; m1.window = 1; m1.a = 1;
	unsigned char pay[8] = {1,2,3,4,5,6,7,8};
	nw_msg m2{}; m2.type = NW_EVT_PASTE; m2.window = 1; m2.length = 8;
	std::vector<unsigned char> bytes = wire(m1, nullptr);
	auto b2 = wire(m2, pay);
	bytes.insert(bytes.end(), b2.begin(), b2.end());
	auto got = run(bytes, 0);                 // all in one chunk
	REQUIRE(got.size() == 2);
	CHECK(got[0].m.type == NW_EVT_FOCUS);
	CHECK(got[1].m.type == NW_EVT_PASTE);
	REQUIRE(got[1].pay.size() == 8);
	CHECK(got[1].pay[7] == 8);
}

TEST_CASE("decode at arbitrary chunk sizes always yields the same messages") {
	unsigned char pay[300];
	for (int i = 0; i < 300; i++) pay[i] = (unsigned char) (i * 7);
	nw_msg m{}; m.type = NW_REQ_COMMIT; m.length = 300;
	auto bytes = wire(m, pay);
	for (size_t chunk = 1; chunk <= bytes.size(); chunk += 3) {
		auto got = run(bytes, chunk);
		REQUIRE(got.size() == 1);
		REQUIRE(got[0].pay.size() == 300);
		CHECK(memcmp(got[0].pay.data(), pay, 300) == 0);
	}
}

TEST_CASE("oversized payload sets overflow, drops cleanly, and resyncs to the next message") {
	unsigned char big[200];
	memset(big, 0xAB, sizeof big);
	nw_msg m1{}; m1.type = NW_REQ_COMMIT; m1.length = 200;   // bigger than the 64-byte buffer
	nw_msg m2{}; m2.type = NW_EVT_CLOSE; m2.window = 9;       // must still be decoded after
	std::vector<unsigned char> bytes = wire(m1, big);
	auto b2 = wire(m2, nullptr);
	bytes.insert(bytes.end(), b2.begin(), b2.end());
	auto got = run(bytes, 7, /*paycap=*/64);
	REQUIRE(got.size() == 2);
	CHECK(got[0].overflow);                   // first message overran the buffer
	CHECK(got[0].m.length == 200);
	CHECK(got[1].m.type == NW_EVT_CLOSE);     // decoder resynced to the boundary
	CHECK(got[1].m.window == 9);
	CHECK_FALSE(got[1].overflow);
}
