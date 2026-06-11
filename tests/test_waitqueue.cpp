#include "doctest.h"
#include "WaitQueue.h"

using namespace kernel;

// A WaitQueue is pure list surgery over Task::waitNext — exercise it without the scheduler.
static Task mk(int id) {
	Task t;
	t.kesp = t.esp0 = 0; t.state = TASK_BLOCKED; t.body = 0; t.id = id;
	t.kstack = 0; t.wakeAt = 0; t.proc = 0; t.waitNext = 0;
	return t;
}

TEST_CASE("WaitQueue: add parks tasks; empty reflects membership") {
	WaitQueue q;
	CHECK(q.empty());
	Task a = mk(1), b = mk(2);
	q.add(&a);
	CHECK(!q.empty());
	q.add(&b);
	// newest first
	CHECK(q.head == &b);
	CHECK(b.waitNext == &a);
	CHECK(a.waitNext == nullptr);
	q.add(nullptr);          // no-op, must not corrupt the list
	CHECK(q.head == &b);
}

TEST_CASE("WaitQueue: remove unlinks head, middle, tail, and is a no-op for absent/null") {
	WaitQueue q;
	Task a = mk(1), b = mk(2), c = mk(3), other = mk(9);
	q.add(&a); q.add(&b); q.add(&c);   // list: c -> b -> a

	q.remove(&b);                      // middle
	CHECK(q.head == &c);
	CHECK(c.waitNext == &a);
	CHECK(b.waitNext == nullptr);      // cleared on removal

	q.remove(&other);                  // not present -> no change
	CHECK(q.head == &c);

	q.remove(&c);                      // head
	CHECK(q.head == &a);
	q.remove(&a);                      // tail / last
	CHECK(q.empty());

	q.remove(&a);                      // already gone -> no-op
	q.remove(nullptr);                 // null -> no-op
	CHECK(q.empty());
}
