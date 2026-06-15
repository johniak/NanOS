#include "doctest.h"
#include <cstdint>
// The pure GDT base-packing helpers live in the x86 cpu dir (not on the host include
// path); include them by relative path — they are plain inline functions, no asm.
#include "../arch/x86/cpu/GdtBase.h"

TEST_CASE("GDT descriptor base packing round-trips") {
	uint8_t d[8] = {0};
	kernel_arch::gdtPackBase(d, 0xDEADBEEF, 0xFFFFF);
	CHECK(kernel_arch::gdtUnpackBase(d) == 0xDEADBEEF);
}

TEST_CASE("gdtPackBase scatters base/limit to the x86 byte layout and preserves flags") {
	uint8_t d[8] = {0};
	d[5] = 0xF2;          // access byte must be left untouched
	d[6] = 0xC0;          // high flag nibble (granularity/size) must be left untouched
	kernel_arch::gdtPackBase(d, 0x12345678, 0xABCDE);
	// base bytes at offsets 2,3,4,7
	CHECK(d[2] == 0x78);
	CHECK(d[3] == 0x56);
	CHECK(d[4] == 0x34);
	CHECK(d[7] == 0x12);
	// limit low 16 at offsets 0,1; high nibble in low nibble of d[6]
	CHECK(d[0] == 0xDE);
	CHECK(d[1] == 0xBC);
	CHECK((d[6] & 0x0F) == 0x0A);
	CHECK((d[6] & 0xF0) == 0xC0);   // flag nibble preserved
	CHECK(d[5] == 0xF2);            // access preserved
	CHECK(kernel_arch::gdtUnpackBase(d) == 0x12345678u);
}

TEST_CASE("gdtPackBase truncates base/limit to their field widths") {
	uint8_t d[8] = {0};
	kernel_arch::gdtPackBase(d, 0xFFFFFFFF, 0x000FFFFF);
	CHECK(kernel_arch::gdtUnpackBase(d) == 0xFFFFFFFFu);
	CHECK((d[6] & 0x0F) == 0x0F);
}
