#include "doctest.h"
#include "Shm.h"        // pulls memory_manager.h (malloc/free) — do NOT add <cstdlib> (linkage clash)
#include <cstring>

using namespace kernel;

// A malloc-backed frame backend standing in for the kernel's physical-frame allocator. Each "frame"
// is a zeroed 4 KiB block whose pointer doubles as its "physical address" (exactly the identity the
// real kernel relies on). A live-frame counter lets the tests prove nothing leaks.
static int g_live;
static unsigned long testAlloc() {
	void* p = malloc(Shm::PAGE);
	if (p) { memset(p, 0, Shm::PAGE); g_live++; }   // zeroed, like the real backend
	return (unsigned long) p;
}
static void testRelease(unsigned long f) {
	if (f) { g_live--; free((void*) f); }
}
static const ShmFrames TEST_BACKEND = { testAlloc, testRelease };

TEST_CASE("Shm: empty object has no frames and zero size") {
	g_live = 0;
	{
		Shm s(TEST_BACKEND);
		CHECK(s.length() == 0);
		CHECK(s.pages() == 0);
		CHECK(s.frameAt(0) == 0);
	}
	CHECK(g_live == 0);
}

TEST_CASE("Shm: ftruncate rounds up to whole pages and reports the logical size") {
	g_live = 0;
	{
		Shm s(TEST_BACKEND);
		CHECK(s.setSize(4096) == 0);
		CHECK(s.pages() == 1);
		CHECK(s.length() == 4096);
		CHECK(s.frameAt(0) != 0);
		CHECK(s.frameAt(1) == 0);            // beyond the tail

		CHECK(s.setSize(5000) == 0);         // 5000 -> 2 pages
		CHECK(s.pages() == 2);
		CHECK(s.length() == 5000);
		CHECK(s.frameAt(1) != 0);
	}
	CHECK(g_live == 0);                       // destructor released every frame
}

TEST_CASE("Shm: shrink frees the tail frames") {
	g_live = 0;
	Shm s(TEST_BACKEND);
	CHECK(s.setSize(4 * 4096) == 0);
	CHECK(s.pages() == 4);
	CHECK(g_live == 4);
	CHECK(s.setSize(4096) == 0);              // shrink to 1 page
	CHECK(s.pages() == 1);
	CHECK(g_live == 1);                       // three tail frames freed immediately
	CHECK(s.length() == 4096);
}

TEST_CASE("Shm: write/read round-trips through the frames") {
	Shm s(TEST_BACKEND);
	REQUIRE(s.setSize(4096) == 0);
	const char* msg = "NanOS shared memory";
	unsigned n = (unsigned) strlen(msg);
	CHECK(s.writeAt(0, msg, n) == n);
	char rb[64] = {0};
	CHECK(s.readAt(0, rb, n) == n);
	CHECK(strcmp(rb, msg) == 0);
}

TEST_CASE("Shm: the frame IS the storage — a write is visible through frameAt (shared memory)") {
	Shm s(TEST_BACKEND);
	REQUIRE(s.setSize(4096) == 0);
	s.writeAt(0, "A", 1);
	// A second observer that only holds the frame address (as a mmap of the same frame would) sees
	// the byte the writer stored — this is the cross-process sharing property in miniature.
	volatile char* mapped = (volatile char*) s.frameAt(0);
	CHECK(mapped[0] == 'A');
	mapped[1] = 'B';                          // observer writes back through the frame
    char rb[4] = {0};
	CHECK(s.readAt(0, rb, 2) == 2);
	CHECK(rb[0] == 'A');
	CHECK(rb[1] == 'B');                       // the object sees the observer's write
}

TEST_CASE("Shm: a write never grows past the allocated frames") {
	Shm s(TEST_BACKEND);
	REQUIRE(s.setSize(4096) == 0);            // exactly one page
	char big[8192];
	memset(big, 'x', sizeof big);
	CHECK(s.writeAt(0, big, sizeof big) == 4096);   // clamped to the one allocated page
	CHECK(s.length() == 4096);                       // logical size stays within the frames
	// A write starting beyond the allocated capacity moves nothing.
	CHECK(s.writeAt(4096, "z", 1) == 0);
}

TEST_CASE("Shm: read is clamped to the logical size (ftruncate defines EOF, not the page size)") {
	Shm s(TEST_BACKEND);
	REQUIRE(s.setSize(2) == 0);               // 2-byte file — still one whole page allocated
	CHECK(s.pages() == 1);
	CHECK(s.length() == 2);
	CHECK(s.writeAt(0, "hi", 2) == 2);        // fills the logical extent
	char rb[16];
	CHECK(s.readAt(0, rb, 16) == 2);          // only the 2 valid bytes come back (EOF at 2)
	CHECK(s.readAt(2, rb, 4) == 0);           // at EOF
	CHECK(s.readAt(100, rb, 4) == 0);         // past EOF
}

TEST_CASE("Shm: spans a page boundary correctly") {
	Shm s(TEST_BACKEND);
	REQUIRE(s.setSize(2 * 4096) == 0);
	char pattern[16];
	for (int i = 0; i < 16; i++) pattern[i] = (char) ('a' + i);
	CHECK(s.writeAt(4096 - 8, pattern, 16) == 16);   // straddles the page-0/page-1 boundary
	char rb[16] = {0};
	CHECK(s.readAt(4096 - 8, rb, 16) == 16);
	CHECK(memcmp(rb, pattern, 16) == 0);
}

TEST_CASE("Shm: refcount frees only at the last unref") {
	g_live = 0;
	Shm* s = new Shm(TEST_BACKEND);
	REQUIRE(s->setSize(4096) == 0);
	s->ref();                                  // now 2 refs (e.g. after a fork)
	CHECK(s->unref() == false);                // one ref remains — do not delete
	CHECK(g_live == 1);
	CHECK(s->unref() == true);                 // last ref gone
	delete s;
	CHECK(g_live == 0);
}
