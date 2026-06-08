#include "doctest.h"
#include "Heap.h"
#include <cstring>

using namespace kernel;

// A fresh Heap over a private arena (8-aligned). Static so the address is stable.
static Heap* fresh(unsigned size = 64 * 1024) {
	static char arena[256 * 1024];
	Heap* h = new Heap();
	h->init(arena, size);
	return h;
}

TEST_CASE("alloc returns writable, distinct, aligned blocks") {
	Heap* h = fresh();
	void* a = h->alloc(100);
	void* b = h->alloc(100);
	CHECK(a != nullptr);
	CHECK(b != nullptr);
	CHECK(a != b);
	CHECK(((uintptr_t) a & 7u) == 0);   // 8-aligned payload
	CHECK(((uintptr_t) b & 7u) == 0);
	memset(a, 0xAB, 100);               // fully writable
	memset(b, 0xCD, 100);
	CHECK(((unsigned char*) a)[99] == 0xAB);
	delete h;
}

TEST_CASE("free reclaims: a freed block of the same size is reused") {
	Heap* h = fresh();
	void* a = h->alloc(64);
	h->free(a);
	void* b = h->alloc(64);
	CHECK(b == a);                      // same slot handed back
	delete h;
}

TEST_CASE("free coalesces adjacent blocks so a large alloc fits again") {
	Heap* h = fresh(4096);
	unsigned avail = h->freeBytes();
	void* a = h->alloc(1000);
	void* b = h->alloc(1000);
	void* c = h->alloc(1000);
	CHECK(c != nullptr);
	// Free all three; coalescing must rebuild (nearly) the whole arena as one block.
	h->free(b);
	h->free(a);
	h->free(c);
	CHECK(h->freeBytes() == avail);     // fully coalesced back
	void* big = h->alloc(2500);         // wouldn't fit without coalescing the 3 holes
	CHECK(big != nullptr);
	delete h;
}

TEST_CASE("alloc splits a large free block and leaves the remainder usable") {
	Heap* h = fresh(4096);
	void* big = h->alloc(8);            // tiny request out of the big block -> split
	unsigned freeAfter = h->freeBytes();
	CHECK(freeAfter > 3000);            // most of the arena is still free
	void* more = h->alloc(2000);
	CHECK(more != nullptr);
	(void) big;
	delete h;
}

TEST_CASE("realloc grows (copying) and shrinks (in place)") {
	Heap* h = fresh();
	char* a = (char*) h->alloc(16);
	memcpy(a, "hello world!!!!", 16);
	char* b = (char*) h->realloc(a, 4096);   // must move
	CHECK(b != nullptr);
	CHECK(memcmp(b, "hello world!!!!", 16) == 0);   // contents preserved
	void* c = h->realloc(b, 8);                      // shrink fits in place
	CHECK(c == b);
	delete h;
}

TEST_CASE("realloc(NULL) == alloc, realloc(p,0) frees") {
	Heap* h = fresh();
	void* a = h->realloc(nullptr, 32);
	CHECK(a != nullptr);
	void* r = h->realloc(a, 0);
	CHECK(r == nullptr);
	// The block was freed, so it can be reallocated.
	void* b = h->alloc(32);
	CHECK(b == a);
	delete h;
}

TEST_CASE("free(NULL) and double-free are ignored (no corruption)") {
	Heap* h = fresh();
	h->free(nullptr);
	void* a = h->alloc(64);
	unsigned f1 = h->freeBytes();
	h->free(a);
	unsigned f2 = h->freeBytes();
	h->free(a);                         // double free: must be ignored
	CHECK(h->freeBytes() == f2);        // no change (not freed twice)
	CHECK(f2 > f1);
	// Allocator still works after the double-free attempt.
	void* b = h->alloc(64);
	CHECK(b != nullptr);
	delete h;
}

TEST_CASE("alloc returns 0 when the arena is exhausted") {
	Heap* h = fresh(4096);
	void* big = h->alloc(8192);         // larger than the arena
	CHECK(big == nullptr);
	// Drain the arena with fixed-size allocs until it returns 0.
	int n = 0;
	while (h->alloc(256) != nullptr && n < 1000)
		n++;
	CHECK(n > 0);
	CHECK(h->alloc(256) == nullptr);
	delete h;
}
