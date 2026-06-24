/*
 * Pipe.h
 *
 * An in-kernel byte FIFO — the object behind a pipe() pair (and the model the PTY reuses
 * in Stage 2). A fixed ring buffer with separate open-end refcounts so EOF is reported
 * correctly: a read on an empty pipe returns 0 (EOF) once ALL write ends are closed,
 * otherwise it would block (the caller / dispatch decides). Pure data structure — no
 * scheduler/arch dependency — so it is host-testable; blocking lives in the dispatch.
 */
#pragma once

#include "WaitQueue.h"
#include "Spinlock.h"   // SMP: a reader thread and a writer thread can hit one pipe on two CPUs

namespace kernel {

class Pipe {
public:
	Pipe() : m_head(0), m_tail(0), m_count(0), m_readers(0), m_writers(0) {}

	// Tasks blocked reading (empty) or writing (full) this pipe park here; the dispatch wakes
	// them after any read/write/close changes readiness, instead of re-polling every tick.
	WaitQueue* waitQueue() { return &m_wq; }

	// Open-end refcounts: pipe() opens one of each; dup() bumps; close() drops. SMP: refcount RMWs
	// and the ring are guarded by m_lock. The simple state queries (readable/writable/atEof) stay
	// lock-free single-int reads — the dispatch re-checks them under the sleepOn IRQ-off recheck, so
	// a torn/stale answer only costs a recheck, never a lost wakeup.
	void addReader() { SpinGuard g(m_lock); m_readers++; }
	void addWriter() { SpinGuard g(m_lock); m_writers++; }
	void dropReader() { SpinGuard g(m_lock); if (m_readers > 0) m_readers--; }
	void dropWriter() { SpinGuard g(m_lock); if (m_writers > 0) m_writers--; }
	int readers() const { return m_readers; }
	int writers() const { return m_writers; }

	bool readable() const { return m_count > 0; }
	bool writable() const { return m_count < CAP; }
	bool atEof() const { return m_count == 0 && m_writers == 0; }  // drained + no writer left

	// Move up to n bytes into the ring. Returns bytes written (0 if full -> caller blocks).
	int write(const void* src, unsigned n) {
		SpinGuard g(m_lock);
		const unsigned char* s = (const unsigned char*) src;
		int w = 0;
		while ((unsigned) w < n && m_count < CAP) {
			m_buf[m_head] = s[w++];
			m_head = (m_head + 1) % CAP;
			m_count++;
		}
		return w;
	}

	// Move up to n bytes out of the ring. Returns bytes read (0 if empty -> EOF or block).
	int read(void* dst, unsigned n) {
		SpinGuard g(m_lock);
		unsigned char* d = (unsigned char*) dst;
		int r = 0;
		while ((unsigned) r < n && m_count > 0) {
			d[r++] = m_buf[m_tail];
			m_tail = (m_tail + 1) % CAP;
			m_count--;
		}
		return r;
	}

private:
	// 64 KiB, not 4 KiB: a windowed app's framebuffer COMMIT (e.g. a full vim redraw, ~770 KiB of
	// pixels through the request pipe) otherwise ping-pongs ~190 times against a 4 KiB ring —
	// each block waits a scheduler round-trip, which is brutally slow under QEMU TCG. A larger
	// ring cuts the round-trips ~16x. Pipes are heap-allocated and few, so the extra bytes are cheap.
	static const int CAP = 64 * 1024;
	unsigned char m_buf[CAP];
	int m_head, m_tail, m_count;
	int m_readers, m_writers;
	WaitQueue m_wq;
	mutable Spinlock m_lock;   // guards the ring + refcounts (reader/writer on different CPUs)
};

}  // namespace kernel
