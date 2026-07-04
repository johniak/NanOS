// Host doctests for the RCU grace-period predicate (Scheduler::rcuGraceDone). The predicate decides
// when synchronize_rcu may return: every OTHER online CPU must have context-switched (now != snap)
// or be idle right now — either state proves it holds no pre-existing RCU reader (see the deferred-
// preemption argument in Scheduler::rcuSynchronize and linuxkpi/include/linux/rcupdate.h).
#include "doctest.h"
#include "Scheduler.h"

using kernel::Scheduler;

TEST_CASE("rcuGraceDone: a still, non-idle peer keeps the grace period open") {
	unsigned snap[4]  = { 5, 10, 0, 0 };
	unsigned now[4]   = { 5, 10, 0, 0 };
	bool     idle[4]  = { false, false, false, false };
	bool     online[4]= { true, true, false, false };
	// self = CPU 0; CPU 1 is online, has not switched and is not idle -> not done.
	CHECK(Scheduler::rcuGraceDone(snap, now, idle, online, 4, 0) == false);
}

TEST_CASE("rcuGraceDone: a peer context switch closes the grace period") {
	unsigned snap[4]  = { 5, 10, 0, 0 };
	unsigned now[4]   = { 5, 11, 0, 0 };   // CPU 1 switched once
	bool     idle[4]  = { false, false, false, false };
	bool     online[4]= { true, true, false, false };
	CHECK(Scheduler::rcuGraceDone(snap, now, idle, online, 4, 0) == true);
}

TEST_CASE("rcuGraceDone: an idle peer counts as quiesced") {
	unsigned snap[4]  = { 5, 10, 0, 0 };
	unsigned now[4]   = { 5, 10, 0, 0 };   // CPU 1 did not switch...
	bool     idle[4]  = { false, true, false, false };   // ...but is idle now
	bool     online[4]= { true, true, false, false };
	CHECK(Scheduler::rcuGraceDone(snap, now, idle, online, 4, 0) == true);
}

TEST_CASE("rcuGraceDone: the caller's own CPU and offline CPUs are ignored") {
	unsigned snap[4]  = { 0, 0, 0, 0 };
	unsigned now[4]   = { 0, 0, 0, 0 };
	bool     idle[4]  = { false, false, false, false };
	bool     online[4]= { true, true, true, false };
	// self = CPU 1 (ignored). CPU 0 and CPU 2 are still & non-idle -> not done; CPU 3 offline.
	CHECK(Scheduler::rcuGraceDone(snap, now, idle, online, 4, 1) == false);
	now[0] = 1; now[2] = 1;   // both peers switch
	CHECK(Scheduler::rcuGraceDone(snap, now, idle, online, 4, 1) == true);
}
