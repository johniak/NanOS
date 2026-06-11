/*
 * WaitQueue.h — an intrusive list of tasks parked waiting for an event (data on a pipe/pty,
 * a key on the console). One WaitQueue lives on each shared, blockable object; a reader or
 * writer that would block sleeps on it (Scheduler::sleepOn), and any operation that changes
 * the object's readiness wakes everyone parked there (Scheduler::wakeAll), which re-test their
 * condition. This replaces 1 kHz tick-polling with event-driven wakeups.
 *
 * Pure data structure (just list surgery over Task::waitNext) — no scheduler/arch — so it is
 * host-testable; the actual sleep/wake bridge lives in Scheduler. A task is on at most one
 * WaitQueue at a time (it always removes itself before returning from sleepOn).
 */
#ifndef WAITQUEUE_H_
#define WAITQUEUE_H_

#include "Scheduler.h"   // struct Task (+ Task::waitNext)

namespace kernel {

struct WaitQueue {
	Task* head = 0;   // singly-linked via Task::waitNext, newest first (order is irrelevant)

	// Park `t` at the front. Caller guarantees `t` is not already on a queue.
	void add(Task* t) {
		if (!t) return;
		t->waitNext = head;
		head = t;
	}

	// Unlink `t` if present (a no-op otherwise). O(n) over the (short) parked list.
	void remove(Task* t) {
		if (!t) return;
		Task** pp = &head;
		while (*pp) {
			if (*pp == t) { *pp = t->waitNext; t->waitNext = 0; return; }
			pp = &(*pp)->waitNext;
		}
	}

	bool empty() const { return head == 0; }
};

}  // namespace kernel

#endif /* WAITQUEUE_H_ */
