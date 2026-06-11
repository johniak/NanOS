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

TEST_CASE("wake only revives a BLOCKED task; never resurrects a zombie/done/stopped") {
	Task t;
	t.kesp = t.esp0 = 0; t.body = 0; t.id = 1; t.kstack = 0; t.wantTick = false;

	// BLOCKED -> READY (the legitimate wakeup).
	t.state = TASK_BLOCKED;
	Scheduler::wake(&t);
	CHECK(t.state == TASK_READY);

	// Every other state is untouched: waking must not make a dead/stopped task runnable again.
	const TaskState untouched[] = { TASK_ZOMBIE, TASK_DONE, TASK_STOPPED, TASK_FREE,
	                                TASK_READY, TASK_RUNNING };
	for (TaskState s : untouched) {
		t.state = s;
		Scheduler::wake(&t);
		CHECK(t.state == s);
	}

	// A null task is a no-op (procExit may wake a parent that no longer exists).
	Scheduler::wake(0);   // must not crash
}

TEST_CASE("resume un-stops only a STOPPED task (SIGCONT), distinct from wake") {
	Task t;
	t.kesp = t.esp0 = 0; t.body = 0; t.id = 1; t.kstack = 0; t.wantTick = false;

	// STOPPED -> READY (the job-control continue path).
	t.state = TASK_STOPPED;
	Scheduler::resume(&t);
	CHECK(t.state == TASK_READY);

	// resume() must not touch a blocked/dead task — only a stop is continued this way.
	const TaskState untouched[] = { TASK_BLOCKED, TASK_ZOMBIE, TASK_DONE, TASK_FREE,
	                                TASK_READY, TASK_RUNNING };
	for (TaskState s : untouched) {
		t.state = s;
		Scheduler::resume(&t);
		CHECK(t.state == s);
	}
	Scheduler::resume(0);   // null is a no-op

	// wake() must NOT resume a stopped task (a child's SIGCHLD can't un-stop a Ctrl+Z'd proc).
	t.state = TASK_STOPPED;
	Scheduler::wake(&t);
	CHECK(t.state == TASK_STOPPED);
}

TEST_CASE("loadDecay: blends toward the runnable count over successive 5s steps") {
	unsigned load[3] = { 0, 0, 0 };
	// With a steady runnable count, each EWMA rises toward it; the 1-min average rises
	// fastest. After one step from 0 with runnable=2, load1 should be > load5 > load15.
	loadDecay(load, 2);
	CHECK(load[0] > load[1]);
	CHECK(load[1] >= load[2]);
	// Idle (runnable=0) decays everything back toward 0.
	unsigned before = load[0];
	loadDecay(load, 0);
	CHECK(load[0] < before);
	// Sustained load converges near runnable*FIXED_1 (2*2048=4096); never overshoots it.
	for (int i = 0; i < 200; i++) loadDecay(load, 2);
	CHECK(load[0] <= 2u * 2048u);
	CHECK(load[0] > 2u * 2048u - 50u);   // within ~1% of the target
}
