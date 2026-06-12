#include "doctest.h"
#include "Net.h"
#include <cstdint>

using namespace kernel;

// The canonical IPv4-header checksum example (Wikipedia "IPv4 header checksum"):
//   4500 0073 0000 4000 4011 b861 c0a8 0001 c0a8 00c7
// With the checksum field present, the whole header sums to 0 (valid). With it zeroed, the
// computed checksum is 0xb861.
static const unsigned char IP_HDR_WITH_CSUM[20] = {
	0x45,0x00,0x00,0x73, 0x00,0x00,0x40,0x00, 0x40,0x11,0xb8,0x61,
	0xc0,0xa8,0x00,0x01, 0xc0,0xa8,0x00,0xc7
};
static const unsigned char IP_HDR_ZERO_CSUM[20] = {
	0x45,0x00,0x00,0x73, 0x00,0x00,0x40,0x00, 0x40,0x11,0x00,0x00,
	0xc0,0xa8,0x00,0x01, 0xc0,0xa8,0x00,0xc7
};

TEST_CASE("inetChecksum: valid header re-checksums to zero") {
	CHECK(inetChecksum(IP_HDR_WITH_CSUM, 20) == 0x0000);
}

TEST_CASE("inetChecksum: computed value for the canonical header is 0xb861") {
	CHECK(inetChecksum(IP_HDR_ZERO_CSUM, 20) == 0xb861);
}

TEST_CASE("inetChecksum: odd length pads the final byte high") {
	// One byte 0x00 -> word 0x0000 -> ~0 = 0xffff. One byte 0xff -> word 0xff00 -> ~0xff00 = 0x00ff.
	unsigned char z = 0x00, f = 0xff;
	CHECK(inetChecksum(&z, 1) == 0xffff);
	CHECK(inetChecksum(&f, 1) == 0x00ff);
}

TEST_CASE("inetChecksum: carry folding (all-ones words)") {
	unsigned char ones[4] = { 0xff, 0xff, 0xff, 0xff };
	// 0xffff + 0xffff = 0x1fffe -> fold -> 0xffff -> ~ = 0x0000.
	CHECK(inetChecksum(ones, 4) == 0x0000);
}

TEST_CASE("inetChecksumAccum chains across regions (pseudo-header style)") {
	// Splitting a buffer and accumulating must equal a single-shot checksum.
	uint32_t s = inetChecksumAccum(IP_HDR_ZERO_CSUM, 10, 0);
	s = inetChecksumAccum(IP_HDR_ZERO_CSUM + 10, 10, s);
	CHECK(inetChecksumFinish(s) == 0xb861);
}

TEST_CASE("byte-order helpers") {
	CHECK(hton16(0x1234) == 0x3412);
	CHECK(ntoh16(0x3412) == 0x1234);
	CHECK(hton32(0x12345678u) == 0x78563412u);
	CHECK(ntoh32(0x78563412u) == 0x12345678u);
	unsigned char b[4];
	wr32be(b, 0xC0A80001u);
	CHECK(b[0] == 0xc0); CHECK(b[1] == 0xa8); CHECK(b[2] == 0x00); CHECK(b[3] == 0x01);
	CHECK(rd32be(b) == 0xC0A80001u);
	CHECK(ipv4(192,168,0,1) == 0xC0A80001u);
}
