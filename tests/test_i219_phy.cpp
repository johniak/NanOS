#include "doctest.h"
#include "i219_phy.h"

TEST_CASE("mdicCmd encodes the e1000 MDIC read/write command word") {
	// READ: opcode 2<<26, phyAddr 1<<21, reg 2<<16, no data.
	uint32_t r = mdicCmd(false, 1, 2, 0);
	CHECK((r & 0x0C000000u) == 0x08000000u);          // OP = READ (2 << 26)
	CHECK(((r >> 21) & 0x1Fu) == 1u);                 // PHY address
	CHECK(((r >> 16) & 0x1Fu) == 2u);                 // PHY register
	CHECK((r & 0xFFFFu) == 0u);                        // a read carries no data
	// WRITE: opcode 1<<26, data in the low 16 bits.
	uint32_t w = mdicCmd(true, 1, 0, 0xABCD);
	CHECK((w & 0x0C000000u) == 0x04000000u);          // OP = WRITE (1 << 26)
	CHECK((w & 0xFFFFu) == 0xABCDu);                   // write data
	CHECK(((w >> 21) & 0x1Fu) == 1u);
}
