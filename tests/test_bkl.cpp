#include "doctest.h"
#include "Bkl.h"
using namespace kernel;

// The host harness is single-CPU (arch::smpThisCpu() == 0), so these exercise the recursion
// bookkeeping: the lock is taken once on the outer enter and released once on the matching
// outer exit, with nested enter/exit only adjusting depth.

TEST_CASE("BKL is recursive on one CPU: nested enter/exit releases exactly once") {
    Bkl b;
    CHECK(b.ownerCpu == -1);
    CHECK(b.depth == 0);

    b.enter();                        // outer: actually takes the spinlock
    CHECK(b.ownerCpu == 0);
    CHECK(b.depth == 1);
    CHECK(b.heldByThisCpu());

    b.enter();                        // nested (e.g. fault inside a syscall): just depth++
    CHECK(b.depth == 2);
    CHECK(b.ownerCpu == 0);

    b.exit();                         // inner release: still held
    CHECK(b.depth == 1);
    CHECK(b.ownerCpu == 0);
    CHECK(b.heldByThisCpu());

    b.exit();                         // outer release: lock freed
    CHECK(b.depth == 0);
    CHECK(b.ownerCpu == -1);
    CHECK_FALSE(b.heldByThisCpu());
}

TEST_CASE("BKL can be re-acquired after a full release (lock was actually freed)") {
    Bkl b;
    b.enter();
    b.exit();
    CHECK(b.ownerCpu == -1);
    // If exit() had not unlocked the underlying spinlock, this second cycle would spin forever.
    b.enter();
    CHECK(b.depth == 1);
    CHECK(b.ownerCpu == 0);
    b.exit();
    CHECK(b.depth == 0);
    CHECK(b.ownerCpu == -1);
}

TEST_CASE("deeply nested re-entry balances") {
    Bkl b;
    for (int i = 0; i < 5; i++) b.enter();
    CHECK(b.depth == 5);
    CHECK(b.ownerCpu == 0);
    for (int i = 0; i < 4; i++) b.exit();
    CHECK(b.depth == 1);
    CHECK(b.ownerCpu == 0);            // still held until the last exit
    b.exit();
    CHECK(b.ownerCpu == -1);
}
