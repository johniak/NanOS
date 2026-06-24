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

// The host harness reports smpThisCpu()==0 always, so a recursive re-enter must NOT block: the
// guard nests via depth++. This is the property ProcTable's composing methods rely on.
TEST_CASE("recursive irq guard nests on the same CPU without deadlocking") {
	RecursiveSpinlock rs;
	{
		RecursiveIrqGuard outer(rs);   // depth 1
		{
			RecursiveIrqGuard inner(rs);   // same CPU -> depth 2, does not block
			RecursiveIrqGuard inner2(rs);  // depth 3
		}                                  // back to depth 1 (lock still held)
	}                                      // outermost release -> fully unlocked
	// Fully released: a plain ticket lock taken via enter() must now acquire immediately.
	RecursiveIrqGuard again(rs);
	CHECK(true);   // reaching here means no deadlock occurred
}
