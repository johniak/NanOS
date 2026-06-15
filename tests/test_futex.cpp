#include "doctest.h"
#include "Futex.h"

using namespace kernel;

TEST_CASE("FUTEX_WAIT returns -EAGAIN when the value already changed") {
	unsigned word = 7;
	CHECK(kernel::futexWaitPrecheck(&word, /*expected*/5) == -11);  // 7 != 5 -> -EAGAIN
	CHECK(kernel::futexWaitPrecheck(&word, /*expected*/7) == 0);    // matches -> would block
}

TEST_CASE("popOne unlinks the oldest matching waiter (FIFO), 0 when none") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	void* B = (void*)0x2000;
	FutexWaiter a{}, b{}, other{};
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &b);
	ft.enqueue(sp, B, &other);            // different key, must be untouched
	CHECK(ft.popOne(sp, A) == &a);        // oldest first
	CHECK(ft.popOne(sp, A) == &b);
	CHECK(ft.popOne(sp, A) == (FutexWaiter*)0);   // drained -> 0
	CHECK(ft.count(sp, B) == 1);                  // other key intact
	CHECK(ft.popOne(sp, B) == &other);
	// popped nodes are unlinked (next cleared)
	CHECK(a.next == (FutexWaiter*)0);
}

TEST_CASE("popOneBitset matches only intersecting masks, never a bitset==0 waiter") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	FutexWaiter z{}, a{}, b{};
	z.bitset = 0; a.bitset = 0x1; b.bitset = 0x2;
	ft.enqueue(sp, A, &z);
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &b);
	CHECK(ft.popOneBitset(sp, A, 0x1) == &a);             // skips z (0), matches a
	CHECK(ft.popOneBitset(sp, A, 0x4) == (FutexWaiter*)0);// nothing intersects 0x4 (b is 0x2)
	CHECK(ft.popOneBitset(sp, A, 0x2) == &b);
	CHECK(ft.count(sp, A) == 1);                          // only z remains
	CHECK(ft.popOneBitset(sp, A, ~0u) == (FutexWaiter*)0);// MATCH_ANY still never wakes z (0)
	CHECK(ft.popOne(sp, A) == &z);                        // plain pop catches it
}

TEST_CASE("futex buckets keyed by (space, addr): independent spaces, wake N, requeue") {
	kernel::FutexTable ft;
	void* sp1 = (void*)0xA00; void* sp2 = (void*)0xB00;   // two address spaces
	void* A = (void*)0x1000; void* B = (void*)0x2000;
	kernel::FutexWaiter w1{}, w2{}, w3{}, wx{};
	ft.enqueue(sp1, A, &w1); ft.enqueue(sp1, A, &w2); ft.enqueue(sp1, A, &w3);
	ft.enqueue(sp2, A, &wx);                                // same VA, different space
	CHECK(ft.wake(sp1, A, 2) == 2);          // wakes 2 of sp1's 3 (FIFO), not sp2's
	CHECK(ft.count(sp1, A) == 1);
	CHECK(ft.count(sp2, A) == 1);            // untouched — different key
	CHECK(ft.requeue(sp1, A, sp1, B, /*wake*/0, /*move*/100) == 1); // move sp1's remaining A-waiter to B
	CHECK(ft.count(sp1, A) == 0);
	CHECK(ft.count(sp1, B) == 1);
}

TEST_CASE("enqueue stamps the key fields and links FIFO") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	FutexWaiter a{}, b{};
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &b);
	CHECK(a.space == sp);
	CHECK(a.uaddr == A);
	CHECK(b.space == sp);
	CHECK(ft.count(sp, A) == 2);
	// FIFO: waking 1 takes `a` first, leaving `b`.
	CHECK(ft.wake(sp, A, 1) == 1);
	CHECK(ft.count(sp, A) == 1);
	CHECK(ft.wake(sp, A, 1) == 1);
	CHECK(ft.count(sp, A) == 0);
}

TEST_CASE("enqueue ignores a null waiter") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	ft.enqueue(sp, A, nullptr);
	CHECK(ft.count(sp, A) == 0);
}

TEST_CASE("wake caps at the number actually parked and at n") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	FutexWaiter a{}, b{};
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &b);
	// Asking for more than parked returns only what was there.
	CHECK(ft.wake(sp, A, 100) == 2);
	CHECK(ft.count(sp, A) == 0);
	// Waking an empty key returns 0.
	CHECK(ft.wake(sp, A, 1) == 0);
	// n <= 0 wakes nothing.
	ft.enqueue(sp, A, &a);
	CHECK(ft.wake(sp, A, 0) == 0);
	CHECK(ft.count(sp, A) == 1);
}

TEST_CASE("wake only touches the matching key, not others sharing a bucket") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	void* B = (void*)0x2000;
	FutexWaiter a{}, bb{};
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, B, &bb);
	CHECK(ft.wake(sp, A, 10) == 1);
	CHECK(ft.count(sp, A) == 0);
	CHECK(ft.count(sp, B) == 1);   // B-waiter untouched
}

TEST_CASE("wakeBitset only wakes waiters whose mask intersects") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	FutexWaiter a{}, b{}, c{};
	a.bitset = 0x1; b.bitset = 0x2; c.bitset = 0x3;
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &b);
	ft.enqueue(sp, A, &c);
	// mask 0x1 hits a (0x1) and c (0x3), skips b (0x2).
	CHECK(ft.wakeBitset(sp, A, 100, 0x1) == 2);
	CHECK(ft.count(sp, A) == 1);   // only b remains
	// The remaining one is b; mask 0x2 wakes it.
	CHECK(ft.wakeBitset(sp, A, 100, 0x2) == 1);
	CHECK(ft.count(sp, A) == 0);
}

TEST_CASE("wakeBitset honours n and matches FIFO order") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	FutexWaiter a{}, b{}, c{};
	a.bitset = 0xF; b.bitset = 0xF; c.bitset = 0xF;
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &b);
	ft.enqueue(sp, A, &c);
	CHECK(ft.wakeBitset(sp, A, 1, 0xF) == 1);  // only the oldest
	CHECK(ft.count(sp, A) == 2);
	// non-matching mask wakes none
	CHECK(ft.wakeBitset(sp, A, 100, 0x0) == 0);
	CHECK(ft.count(sp, A) == 2);
}

TEST_CASE("a bitset==0 (plain FUTEX_WAIT) waiter is never woken by wakeBitset, only by wake") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	FutexWaiter z{};                            // bitset defaults to 0 -> plain FUTEX_WAIT
	ft.enqueue(sp, A, &z);
	CHECK(ft.wakeBitset(sp, A, 1, ~0u) == 0);   // not even MATCH_ANY matches a bitset==0 waiter
	CHECK(ft.count(sp, A) == 1);
	CHECK(ft.wake(sp, A, 1) == 1);              // plain wake() is the only thing that catches it
	CHECK(ft.count(sp, A) == 0);
}

TEST_CASE("requeue with nwake>0 wakes then moves") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	void* B = (void*)0x2000;
	FutexWaiter a{}, b{}, c{}, d{};
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &b);
	ft.enqueue(sp, A, &c);
	ft.enqueue(sp, A, &d);
	// wake 1 (a), move up to 2 (b, c); d stays on A.
	CHECK(ft.requeue(sp, A, sp, B, 1, 2) == 3);  // 1 woken + 2 moved
	CHECK(ft.count(sp, A) == 1);                 // d
	CHECK(ft.count(sp, B) == 2);                 // b, c (re-keyed)
	// moved waiters got re-keyed
	CHECK(b.space == sp);
	CHECK(b.uaddr == B);
	CHECK(c.uaddr == B);
}

TEST_CASE("requeue across address spaces re-keys both space and addr") {
	FutexTable ft;
	void* sp1 = (void*)0x10;
	void* sp2 = (void*)0x20;
	void* A = (void*)0x1000;
	void* B = (void*)0x2000;
	FutexWaiter a{};
	ft.enqueue(sp1, A, &a);
	CHECK(ft.requeue(sp1, A, sp2, B, 0, 100) == 1);
	CHECK(ft.count(sp1, A) == 0);
	CHECK(ft.count(sp2, B) == 1);
	CHECK(a.space == sp2);
	CHECK(a.uaddr == B);
}

TEST_CASE("requeue with empty source affects nothing") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	void* B = (void*)0x2000;
	CHECK(ft.requeue(sp, A, sp, B, 1, 100) == 0);
	CHECK(ft.count(sp, B) == 0);
}

TEST_CASE("remove unlinks a specific waiter; null and absent are no-ops") {
	FutexTable ft;
	void* sp = (void*)0x10;
	void* A = (void*)0x1000;
	FutexWaiter a{}, b{}, c{}, loose{};
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &b);
	ft.enqueue(sp, A, &c);
	// remove the middle one
	ft.remove(&b);
	CHECK(ft.count(sp, A) == 2);
	// removing one not in the table is harmless
	ft.remove(&loose);
	CHECK(ft.count(sp, A) == 2);
	// null is harmless
	ft.remove(nullptr);
	CHECK(ft.count(sp, A) == 2);
	// remaining FIFO is a then c
	CHECK(ft.wake(sp, A, 100) == 2);
	CHECK(ft.count(sp, A) == 0);
	// remove the head
	ft.enqueue(sp, A, &a);
	ft.enqueue(sp, A, &c);
	ft.remove(&a);
	CHECK(ft.count(sp, A) == 1);
	ft.remove(&c);
	CHECK(ft.count(sp, A) == 0);
}
