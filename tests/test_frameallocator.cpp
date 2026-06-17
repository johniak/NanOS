#include "doctest.h"
#include "FrameAllocator.h"
#include <cstdint>

using namespace kernel;

// Each test gets its own allocator (heap-allocated: the bitmap member is large).
static FrameAllocator* fresh(uint64_t top) {
	FrameAllocator* fa = new FrameAllocator();
	fa->init(top);
	return fa;
}

TEST_CASE("init marks every frame used") {
	FrameAllocator* fa = fresh(0x10000);   // 16 frames
	CHECK(fa->freeCount() == 0);
	CHECK(fa->isUsed(0));
	CHECK(fa->isUsed(15));
	delete fa;
}

TEST_CASE("markRangeFree opens an aligned window; freeCount tracks it") {
	FrameAllocator* fa = fresh(0x10000);
	fa->markRangeFree(0x1000, 0xF000);     // frames 1..15
	CHECK(fa->freeCount() == 15);
	CHECK(fa->isUsed(0));                   // frame 0 stayed reserved
	CHECK(!fa->isUsed(1));
	CHECK(!fa->isUsed(15));
	delete fa;
}

TEST_CASE("bit indexing works across a 32-bit word boundary") {
	FrameAllocator* fa = fresh(0x40000);   // 64 frames (two bitmap words)
	fa->markRangeFree(31 * 0x1000, 3 * 0x1000);   // frames 31,32,33
	CHECK(!fa->isUsed(31));
	CHECK(!fa->isUsed(32));
	CHECK(!fa->isUsed(33));
	CHECK(fa->isUsed(30));
	CHECK(fa->isUsed(34));
	delete fa;
}

TEST_CASE("markRangeFree rounds inward (never frees a partially-covered frame)") {
	FrameAllocator* fa = fresh(0x10000);
	fa->markRangeFree(0x1800, 0x3000);     // [0x1800, 0x4800): ceil->frame2, floor->frame4
	CHECK(fa->isUsed(1));                   // partial start frame stays used
	CHECK(!fa->isUsed(2));
	CHECK(!fa->isUsed(3));
	CHECK(fa->isUsed(4));                   // partial end frame stays used
	delete fa;
}

TEST_CASE("markRangeUsed rounds outward (reserves any touched frame)") {
	FrameAllocator* fa = fresh(0x10000);
	fa->markRangeFree(0, 0x10000);          // free all 16
	fa->markRangeUsed(0x1800, 0x3000);      // [0x1800,0x4800): floor->frame1, ceil->frame5
	CHECK(fa->isUsed(1));
	CHECK(fa->isUsed(2));
	CHECK(fa->isUsed(3));
	CHECK(fa->isUsed(4));
	CHECK(!fa->isUsed(0));
	CHECK(!fa->isUsed(5));
	delete fa;
}

TEST_CASE("alloc returns distinct, page-aligned frames and flips bits") {
	FrameAllocator* fa = fresh(0x10000);
	fa->markRangeFree(0x1000, 0xF000);      // frames 1..15 free
	uint32_t a = fa->alloc();
	uint32_t b = fa->alloc();
	CHECK(a == 0x1000);                     // first free
	CHECK(b == 0x2000);
	CHECK((a & 0xFFF) == 0);
	CHECK(a != b);
	CHECK(fa->isUsed(1));
	CHECK(fa->isUsed(2));
	CHECK(fa->freeCount() == 13);
	delete fa;
}

TEST_CASE("free returns a frame to the pool for reuse") {
	FrameAllocator* fa = fresh(0x10000);
	fa->markRangeFree(0x1000, 0xF000);
	uint32_t a = fa->alloc();               // 0x1000
	fa->free(a);
	CHECK(!fa->isUsed(1));
	CHECK(fa->alloc() == 0x1000);           // handed back out
	delete fa;
}

TEST_CASE("alloc returns 0 (OOM) when no frame is free") {
	FrameAllocator* fa = fresh(0x10000);    // all used, nothing freed
	CHECK(fa->alloc() == 0);
	delete fa;
}

TEST_CASE("allocates a frame above 4 GiB without truncating the high bits (LP64)") {
	// topOfRam = 8 GiB; free a single frame at 5 GiB and allocate it back.
	const uint64_t FIVE_GIB = 5ull * 1024 * 1024 * 1024;
	FrameAllocator* fa = fresh(8ull * 1024 * 1024 * 1024);   // 8 GiB
	fa->markRangeFree(FIVE_GIB, 0x1000);                     // exactly one frame at 5 GiB
	uint64_t a = fa->alloc();
	CHECK(a == FIVE_GIB);                                     // NOT (uint32_t) FIVE_GIB == 0x40000000
	CHECK((a >> 32) != 0);                                    // genuinely a 64-bit address
	delete fa;
}
