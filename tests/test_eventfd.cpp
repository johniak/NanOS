#include "doctest.h"
#include "Eventfd.h"
#include "Epoll.h"

using namespace kernel;

// Eventfd is a pure data structure (counter + refcount + wait queue); the blocking/wakeup lives in
// the dispatch, so here we verify the counter bookkeeping in isolation.

TEST_CASE("Eventfd: non-semaphore read drains the whole counter") {
	Eventfd e(0, /*semaphore=*/false);
	unsigned long long v = 123;
	CHECK(e.read(&v) == 0);              // zero counter: nothing to read
	CHECK(e.write(3) == 8);
	CHECK(e.write(4) == 8);
	CHECK(e.readable());
	CHECK(e.read(&v) == 8);
	CHECK(v == 7);                       // accumulated 3+4, drained at once
	CHECK(!e.readable());
	CHECK(e.read(&v) == 0);              // drained: EAGAIN territory
}

TEST_CASE("Eventfd: initial value is present to the first reader") {
	Eventfd e(9, false);
	unsigned long long v = 0;
	CHECK(e.read(&v) == 8);
	CHECK(v == 9);
}

TEST_CASE("Eventfd: semaphore mode yields 1 per read") {
	Eventfd e(0, /*semaphore=*/true);
	CHECK(e.write(2) == 8);
	unsigned long long v = 0;
	CHECK(e.read(&v) == 8); CHECK(v == 1);
	CHECK(e.read(&v) == 8); CHECK(v == 1);
	CHECK(e.read(&v) == 0);              // now empty
}

TEST_CASE("Eventfd: write refuses to reach the sentinel, accepts after a drain") {
	Eventfd e(0, false);
	CHECK(e.write(Eventfd::MAX) == 8);   // exactly MAX is allowed
	CHECK(!e.writable());                // counter now == MAX -> no room to add
	CHECK(e.write(1) == 0);              // would exceed MAX -> refused (0 == would-block)
	unsigned long long v = 0;
	CHECK(e.read(&v) == 8); CHECK(v == Eventfd::MAX);
	CHECK(e.write(1) == 8);              // after draining, room again
}

TEST_CASE("Eventfd: the 0xffff...ffff value is illegal") {
	Eventfd e(0, false);
	CHECK(e.write(0xffffffffffffffffULL) == -1);   // EINVAL territory
}

TEST_CASE("Eventfd: refcount frees only at the last unref") {
	Eventfd* e = new Eventfd(0, false);
	e->ref();                            // 2 refs
	CHECK(e->unref() == false);          // -> 1
	CHECK(e->unref() == true);           // -> 0 (caller deletes)
	delete e;
}

// ---- Epoll interest set ----------------------------------------------------------------------

TEST_CASE("Epoll: add/mod/del bookkeeping and error codes") {
	Epoll ep;
	CHECK(ep.add(3, 0x001u, 0xAAAA) == 0);
	CHECK(ep.add(3, 0x001u, 0xBBBB) == -17);   // EEXIST
	CHECK(ep.mod(3, 0x004u, 0xCCCC) == 0);
	CHECK(ep.mod(9, 0x001u, 0) == -2);         // ENOENT (fd not registered)
	CHECK(ep.del(3) == 0);
	CHECK(ep.del(3) == -2);                         // ENOENT after removal
}

TEST_CASE("Epoll: snapshot returns exactly the live interests with their data") {
	Epoll ep;
	ep.add(5, 0x001u, 0x1111);
	ep.add(6, 0x004u, 0x2222);
	Epoll::Interest it[Epoll::MAXI];
	int n = ep.snapshot(it, Epoll::MAXI);
	CHECK(n == 2);
	// order is slot order; both fds present with their data
	bool saw5 = false, saw6 = false;
	for (int i = 0; i < n; i++) {
		if (it[i].fd == 5) { saw5 = true; CHECK(it[i].data == 0x1111ULL); }
		if (it[i].fd == 6) { saw6 = true; CHECK(it[i].data == 0x2222ULL); }
	}
	CHECK(saw5); CHECK(saw6);
}

TEST_CASE("Epoll: refcount frees only at the last unref") {
	Epoll* ep = new Epoll();
	ep->ref();
	CHECK(ep->unref() == false);
	CHECK(ep->unref() == true);
	delete ep;
}
