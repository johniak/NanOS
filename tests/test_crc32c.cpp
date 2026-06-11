#include "doctest.h"
#include "ext/Crc32c.h"

using namespace kernel;

// Standard CRC-32C check value: the Castagnoli CRC of "123456789" with init 0xFFFFFFFF and a
// final XOR of 0xFFFFFFFF is 0xE3069283 (the documented check constant for CRC-32C).
TEST_CASE("crc32c matches the CRC-32C check vector for \"123456789\"") {
	unsigned c = crc32c(0xFFFFFFFFu, "123456789", 9) ^ 0xFFFFFFFFu;
	CHECK(c == 0xE3069283u);
}

TEST_CASE("crc32c is continuable: chaining regions == one contiguous region") {
	const char* a = "hello, ";
	const char* b = "world";
	unsigned chained = crc32c(crc32c(0xFFFFFFFFu, a, 7), b, 5);
	unsigned whole   = crc32c(0xFFFFFFFFu, "hello, world", 12);
	CHECK(chained == whole);
}

TEST_CASE("crc32c: empty region leaves the running value unchanged") {
	CHECK(crc32c(0x12345678u, "", 0) == 0x12345678u);
}
