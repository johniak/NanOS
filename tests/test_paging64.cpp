#include "doctest.h"
#include "Paging.h"
#include <cstdint>

using namespace kernel;

TEST_CASE("pml4/pdpt/pd/pt indices split a virtual address (9 bits each)") {
	CHECK(pml4Index(0) == 0);
	CHECK(pdptIndex(0) == 0);
	CHECK(pdIndex(0) == 0);
	CHECK(ptIndex(0) == 0);
	CHECK(pageOffset(0) == 0);

	// 0x400000 = 4 MiB: PD index 2 (2 MiB per PD entry), PT 0, offset 0.
	CHECK(pdIndex(0x400000) == 2);
	CHECK(ptIndex(0x400000) == 0);
	// one page in: PT index 1.
	CHECK(ptIndex(0x401000) == 1);

	// 1 GiB = PDPT index 1; 512 GiB = PML4 index 1.
	CHECK(pdptIndex(0x40000000ULL) == 1);
	CHECK(pml4Index(0x8000000000ULL) == 1);

	// Top of the canonical low half (bit 47 = 0, bits 0..46 set): the sub-PML4 fields
	// saturate to 0x1FF; PML4 (bits 39..47) reaches 0xFF since its top bit (47) is clear.
	uint64_t hi = 0x00007FFFFFFFFFFFULL;   // bit 47 = 0, rest set
	CHECK(pml4Index(hi) == 0xFF);
	CHECK(pdptIndex(hi) == 0x1FF);
	CHECK(pdIndex(hi) == 0x1FF);
	CHECK(ptIndex(hi) == 0x1FF);
	CHECK(pageOffset(hi) == 0xFFF);
}

TEST_CASE("makeEntry / entryAddr round-trip a 40-bit frame address and flags") {
	uint64_t e = makeEntry(0x123456789000ULL, PTE_PRESENT | PTE_RW | PTE_USER);
	CHECK(entryAddr(e) == 0x123456789000ULL);
	CHECK((e & FLAG_MASK) == (PTE_PRESENT | PTE_RW | PTE_USER));
	CHECK((e & PTE_NX) == 0);
}

TEST_CASE("the NX bit lives in bit 63, outside the low flags") {
	uint64_t e = makeEntry(0x1000, PTE_PRESENT | PTE_NX);
	CHECK((e & PTE_NX) != 0);
	CHECK((e & FLAG_MASK) == PTE_PRESENT);          // NX not folded into the low 12 bits
	CHECK(entryAddr(e) == 0x1000u);
}

TEST_CASE("entryPresent reflects the present bit") {
	CHECK(entryPresent(makeEntry(0x1000, PTE_PRESENT)));
	CHECK(!entryPresent(makeEntry(0x1000, PTE_RW)));   // RW without PRESENT
	CHECK(!entryPresent(0));
}

TEST_CASE("canonical addresses sign-extend bit 47") {
	CHECK(isCanonical(0x800000ULL));                       // low window: bit 47 = 0
	CHECK(isCanonical(0x58000000ULL));                     // fb window: still canonical
	CHECK(canonical(0x800000ULL) == 0x800000ULL);          // unchanged
	// A high-half address with bit 47 set canonicalizes by setting bits 48..63.
	CHECK(canonical(0x0000800000000000ULL) == 0xFFFF800000000000ULL);
	CHECK(isCanonical(0xFFFF800000000000ULL));
	CHECK(!isCanonical(0x0000800000000000ULL));            // bit 47 set but high bits clear
}
