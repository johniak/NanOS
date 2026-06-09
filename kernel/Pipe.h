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

namespace kernel {

class Pipe {
public:
	Pipe() : m_head(0), m_tail(0), m_count(0), m_readers(0), m_writers(0) {}

	// Open-end refcounts: pipe() opens one of each; dup() bumps; close() drops.
	void addReader() { m_readers++; }
	void addWriter() { m_writers++; }
	void dropReader() { if (m_readers > 0) m_readers--; }
	void dropWriter() { if (m_writers > 0) m_writers--; }
	int readers() const { return m_readers; }
	int writers() const { return m_writers; }

	bool readable() const { return m_count > 0; }
	bool writable() const { return m_count < CAP; }
	bool atEof() const { return m_count == 0 && m_writers == 0; }  // drained + no writer left

	// Move up to n bytes into the ring. Returns bytes written (0 if full -> caller blocks).
	int write(const void* src, unsigned n) {
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
	static const int CAP = 4096;
	unsigned char m_buf[CAP];
	int m_head, m_tail, m_count;
	int m_readers, m_writers;
};

}  // namespace kernel
