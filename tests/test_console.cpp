#include "doctest.h"
#include "Console.h"
#include <cstring>
#include <cstdint>
using namespace kernel;

TEST_CASE("Console::itoa(uint64_t,16) prints all 64 bits, not the low 32") {
	CHECK(strcmp(Console::itoa((uint64_t) 0x1ABCDEF012345678ull, 16), "1abcdef012345678") == 0);
	CHECK(strcmp(Console::itoa((uint64_t) 0x100000000ull, 16), "100000000") == 0);  // bit 32 set
}
