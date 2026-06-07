#include "doctest.h"
#include "Process.h"

using namespace kernel;

TEST_CASE("ProcTable: alloc gives distinct pids and tracks parent") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	Process* b = ProcTable::alloc(a->pid);
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	CHECK(a->pid != b->pid);
	CHECK(b->parent == a->pid);
	CHECK(a->used);
	CHECK(b->used);
}

TEST_CASE("ProcTable: current() reflects setCurrent; byPid/byTask look up") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	CHECK(ProcTable::current() == nullptr);
	ProcTable::setCurrent(a);
	CHECK(ProcTable::current() == a);
	CHECK(ProcTable::byPid(a->pid) == a);
	CHECK(ProcTable::byPid(9999) == nullptr);

	Task* fake = (Task*) 0x1234;
	a->task = fake;
	CHECK(ProcTable::byTask(fake) == a);
	CHECK(ProcTable::byTask((Task*) 0x5678) == nullptr);
}

TEST_CASE("reapChild: no children -> -ECHILD") {
	ProcTable::init();
	Process* parent = ProcTable::alloc(0);
	Process* out = (Process*) 0x1;
	CHECK(ProcTable::reapChild(parent->pid, -1, &out) == -10);
}

TEST_CASE("reapChild: child alive -> 0; exited -> pid + frees slot") {
	ProcTable::init();
	Process* parent = ProcTable::alloc(0);
	Process* child = ProcTable::alloc(parent->pid);

	// A live (not-yet-exited) child: nothing to reap, but it exists.
	Process* out = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, -1, &out) == 0);
	CHECK(out == nullptr);

	// Now it exits: reapChild returns its pid and hands it back for teardown.
	child->exited = true;
	child->exitCode = 42;
	out = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, -1, &out) == child->pid);
	CHECK(out == child);
	CHECK(out->exitCode == 42);

	// Caller frees the slot; a second wait finds no children.
	ProcTable::freeSlot(out);
	CHECK(!child->used);
	Process* out2 = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, -1, &out2) == -10);
}

TEST_CASE("reapChild: wantPid narrows to a specific child") {
	ProcTable::init();
	Process* parent = ProcTable::alloc(0);
	Process* c1 = ProcTable::alloc(parent->pid);
	Process* c2 = ProcTable::alloc(parent->pid);
	c1->exited = true; c1->exitCode = 1;
	c2->exited = true; c2->exitCode = 2;

	Process* out = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, c2->pid, &out) == c2->pid);
	CHECK(out == c2);
	// A pid that is not a child of parent -> -ECHILD.
	out = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, 9999, &out) == -10);
}
