/*
 * Eventfd.h
 *
 * The kernel object behind eventfd2(2) — a 64-bit counter with wait-queue readiness, used by
 * libuv/Chromium as a self-pipe wakeup for the message pump. Like Pipe, this is a pure data
 * structure (no scheduler/arch dependency) so it is host-testable; blocking + wakeups live in
 * the dispatch (the same tick-rescan model poll() uses).
 *
 * Semantics (Linux eventfd, non-semaphore mode — the only mode libuv/Chromium use):
 *   - write(v): counter += v, saturating-refused if it would reach the 0xffffffffffffffff
 *     sentinel (caller blocks or gets EAGAIN); v == 0xffffffffffffffff is EINVAL.
 *   - read():   returns the whole counter as a u64 and resets it to 0; on a zero counter the
 *     caller blocks or gets EAGAIN.
 *   - EFD_SEMAPHORE (read yields 1 and decrements) is implemented for completeness.
 *   - poll: POLLIN when counter > 0, POLLOUT when counter < MAX (always writable in practice).
 *
 * Refcounted like a pipe end: eventfd2() opens one ref; dup()/fork() bump; close() drops; the
 * object frees at the last close.
 */
#pragma once

#include "WaitQueue.h"
#include "Spinlock.h"

namespace kernel {

class Eventfd {
public:
	// Largest legal counter value; 0xffffffffffffffff is reserved (write of it is EINVAL, and a
	// counter may never reach it — a write that would is refused so the reader can drain first).
	static constexpr unsigned long long MAX = 0xfffffffffffffffeULL;

	explicit Eventfd(unsigned long long initval, bool semaphore)
		: m_count(initval), m_semaphore(semaphore), m_refs(1) {}

	WaitQueue* waitQueue() { return &m_wq; }

	void ref()   { SpinGuard g(m_lock); m_refs++; }
	// Drop one ref; returns true when the last ref went away (caller deletes).
	bool unref() { SpinGuard g(m_lock); return --m_refs == 0; }

	bool readable() const { return m_count > 0; }
	bool writable() const { return m_count < MAX; }   // effectively always writable

	// Try to read the counter. Returns 8 and fills *out on success; returns 0 when the counter is
	// zero (caller blocks or reports EAGAIN). Never partial.
	int read(unsigned long long* out) {
		SpinGuard g(m_lock);
		if (m_count == 0) return 0;
		if (m_semaphore) { *out = 1; m_count -= 1; }
		else             { *out = m_count; m_count = 0; }
		return 8;
	}

	// Try to add `v` to the counter. Returns 8 on success; 0 when it would overflow MAX (caller
	// blocks or reports EAGAIN); -1 when v is the illegal sentinel (caller reports EINVAL).
	int write(unsigned long long v) {
		if (v == 0xffffffffffffffffULL) return -1;
		SpinGuard g(m_lock);
		if (v > MAX - m_count) return 0;   // would reach/exceed the sentinel: refuse, let reader drain
		m_count += v;
		return 8;
	}

private:
	unsigned long long m_count;
	bool m_semaphore;
	int m_refs;
	WaitQueue m_wq;
	mutable Spinlock m_lock;
};

}  // namespace kernel
