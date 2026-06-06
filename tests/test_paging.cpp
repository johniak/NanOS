#include "doctest.h"
#include "Paging.h"
#include <cstdint>

using namespace kernel;

TEST_CASE("pdIndex / ptIndex / pageOffset split a virtual address") {
	CHECK(pdIndex(0) == 0);
	CHECK(ptIndex(0) == 0);
	CHECK(pageOffset(0) == 0);

	// 0x400000 = PD 1, PT 0, offset 0
	CHECK(pdIndex(0x400000) == 1);
	CHECK(ptIndex(0x400000) == 0);
	CHECK(pageOffset(0x400000) == 0);

	// one page in: PD 1, PT 1
	CHECK(pdIndex(0x401000) == 1);
	CHECK(ptIndex(0x401000) == 1);

	// top of the address space
	CHECK(pdIndex(0xFFFFFFFF) == 0x3FF);
	CHECK(ptIndex(0xFFFFFFFF) == 0x3FF);
	CHECK(pageOffset(0xFFFFFFFF) == 0xFFF);
}

TEST_CASE("makeEntry / entryAddr round-trip address and flags") {
	uint32_t e = makeEntry(0x12345678, PTE_PRESENT | PTE_RW | PTE_USER);
	CHECK(entryAddr(e) == 0x12345000u);          // low 12 bits of the addr dropped
	CHECK((e & 0xFFF) == (PTE_PRESENT | PTE_RW | PTE_USER));
}

TEST_CASE("entryPresent reflects the present bit") {
	CHECK(entryPresent(makeEntry(0x1000, PTE_PRESENT)));
	CHECK(!entryPresent(makeEntry(0x1000, PTE_RW)));   // RW without PRESENT
	CHECK(!entryPresent(0));
}
