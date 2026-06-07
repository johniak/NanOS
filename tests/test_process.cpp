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
