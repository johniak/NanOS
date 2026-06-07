#include "doctest.h"
#include "Scheduler.h"

using namespace kernel;

TEST_CASE("nextRunnable: round-robins non-idle tasks, idle (0) only as fallback") {
	TaskState st[3] = { TASK_READY, TASK_READY, TASK_READY };   // 0=idle, 1=A, 2=B
	CHECK(Scheduler::nextRunnable(st, 3, 1) == 2);   // A -> B
	CHECK(Scheduler::nextRunnable(st, 3, 2) == 1);   // B -> A (idle skipped)
	CHECK(Scheduler::nextRunnable(st, 3, 0) == 1);   // idle -> A
}

TEST_CASE("nextRunnable: blocked tasks are skipped; idle when none runnable") {
	TaskState all[3] = { TASK_READY, TASK_BLOCKED, TASK_BLOCKED };
	CHECK(Scheduler::nextRunnable(all, 3, 1) == 0);  // both non-idle blocked -> idle

	TaskState one[3] = { TASK_READY, TASK_READY, TASK_BLOCKED };
	CHECK(Scheduler::nextRunnable(one, 3, 1) == 1);  // only A runnable -> stays A
}

TEST_CASE("nextRunnable: DONE tasks are skipped like blocked") {
	// All non-idle finished -> idle.
	TaskState done[3] = { TASK_READY, TASK_DONE, TASK_DONE };
	CHECK(Scheduler::nextRunnable(done, 3, 1) == 0);

	// A still runnable, B done, current is B -> hand back to A.
	TaskState mix[3] = { TASK_READY, TASK_READY, TASK_DONE };
	CHECK(Scheduler::nextRunnable(mix, 3, 2) == 1);
}
