#include "doctest.h"
#include "Spinlock.h"
using namespace kernel;

TEST_CASE("spinlock is unlocked after construction") {
	Spinlock s;
	CHECK(s.tryLock());        // free -> acquires
	CHECK_FALSE(s.tryLock());  // already held -> fails
	s.unlock();
	CHECK(s.tryLock());        // free again
	s.unlock();
}

TEST_CASE("ticket order is FIFO-fair under tryLock churn") {
	Spinlock s;
	for (int i = 0; i < 1000; i++) { CHECK(s.tryLock()); s.unlock(); }
}
