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

TEST_CASE("pickReady (SMP claim): only TASK_READY non-idle tasks are claimable") {
	// slot 0 = idle, slots 1..3 = work. isIdle[] marks the idle task.
	bool isIdle[4] = { true, false, false, false };
	TaskState st[4] = { TASK_RUNNING, TASK_READY, TASK_READY, TASK_READY };
	// Round-robin from each position, skipping the idle slot.
	CHECK(Scheduler::pickReady(st, isIdle, 4, 0) == 1);
	CHECK(Scheduler::pickReady(st, isIdle, 4, 1) == 2);
	CHECK(Scheduler::pickReady(st, isIdle, 4, 3) == 1);   // wraps past idle(0) to 1
}

TEST_CASE("pickReady: a task already RUNNING is NOT claimable -> two CPUs never pick the same") {
	bool isIdle[4] = { true, false, false, false };
	// CPU A claimed slot 1 (now RUNNING). CPU B scanning from 0 must skip it and take slot 2.
	TaskState st[4] = { TASK_RUNNING, TASK_RUNNING, TASK_READY, TASK_READY };
	CHECK(Scheduler::pickReady(st, isIdle, 4, 0) == 2);
	// Now slots 1 and 2 are both RUNNING (claimed by A and B). A third CPU gets slot 3.
	st[2] = TASK_RUNNING;
	CHECK(Scheduler::pickReady(st, isIdle, 4, 0) == 3);
	// All work RUNNING -> nothing claimable (caller falls back to its per-CPU idle).
	st[3] = TASK_RUNNING;
	CHECK(Scheduler::pickReady(st, isIdle, 4, 0) == -1);
}

TEST_CASE("pickReady: blocked/done work is skipped; -1 when nothing is READY") {
	bool isIdle[3] = { true, false, false };
	TaskState st[3] = { TASK_RUNNING, TASK_BLOCKED, TASK_DONE };
	CHECK(Scheduler::pickReady(st, isIdle, 3, 0) == -1);
	st[1] = TASK_READY;
	CHECK(Scheduler::pickReady(st, isIdle, 3, 0) == 1);
}

TEST_CASE("wake only revives a BLOCKED task; never resurrects a zombie/done/stopped") {
	Task t;
	t.kesp = t.esp0 = 0; t.body = 0; t.id = 1; t.kstack = 0; t.wakeAt = 0;

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
	t.kesp = t.esp0 = 0; t.body = 0; t.id = 1; t.kstack = 0; t.wakeAt = 0;

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

TEST_CASE("timedWakeReady: fires at/after the deadline, never before; 0 = no timer (wrap-safe)") {
	CHECK(!Scheduler::timedWakeReady(100, 0));      // 0 = no timer armed -> never ready
	CHECK(!Scheduler::timedWakeReady(99, 100));     // before the deadline
	CHECK(Scheduler::timedWakeReady(100, 100));     // at the deadline
	CHECK(Scheduler::timedWakeReady(105, 100));     // past the deadline
	// Tick counter wrapped past 2^32: now just after wrap, deadline just before -> still due.
	CHECK(Scheduler::timedWakeReady(5, 0xFFFFFFF0u));
	CHECK(!Scheduler::timedWakeReady(0xFFFFFFF0u, 5));   // deadline is "ahead" across the wrap
}

TEST_CASE("shouldResched: keep the CPU until the quantum expires, but yield to a woken sleeper") {
	CHECK(!Scheduler::shouldResched(1, 10, false));    // mid-quantum, nobody woke -> keep running
	CHECK(!Scheduler::shouldResched(9, 10, false));
	CHECK(Scheduler::shouldResched(10, 10, false));    // quantum elapsed -> reschedule
	CHECK(Scheduler::shouldResched(11, 10, false));
	CHECK(Scheduler::shouldResched(1, 10, true));      // a sleeper woke -> preempt immediately
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
