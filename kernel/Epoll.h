/*
 * Epoll.h
 *
 * The kernel object behind epoll_create1(2). It is a pure interest set: an array of
 * { fd, events, user-data } registrations. Readiness scanning and blocking live in Syscalls
 * (which owns the fd table) using the same per-fd readiness logic as pollScan(), on the same
 * tick-rescan wait model poll() uses — level-triggered, which is what libuv/Chromium's message
 * pump relies on. EPOLLET is a documented follow-up (add only if a probe fails without it).
 *
 * Bounded by the per-process fd table (MAXFD): a process can register at most that many fds, so
 * the fixed array never overflows in practice. Refcounted like the other fd-backed objects.
 */
#pragma once

#include "Spinlock.h"

namespace kernel {

class Epoll {
public:
	struct Interest {
		bool     used;
		int      fd;
		unsigned events;             // EPOLLIN/EPOLLOUT/... requested by the caller
		unsigned long long data;     // opaque epoll_event.data.u64 handed back on readiness
	};
	static const int MAXI = 128;     // == Syscalls::MAXFD: an fd can appear at most once

	Epoll() : m_refs(1) {
		for (int i = 0; i < MAXI; i++) m_it[i].used = false;
	}

	void ref()   { SpinGuard g(m_lock); m_refs++; }
	bool unref() { SpinGuard g(m_lock); return --m_refs == 0; }

	// EPOLL_CTL_ADD: register fd. Returns 0, -EEXIST if already present, -ENOSPC if full.
	int add(int fd, unsigned events, unsigned long long data) {
		SpinGuard g(m_lock);
		int free = -1;
		for (int i = 0; i < MAXI; i++) {
			if (m_it[i].used && m_it[i].fd == fd) return -17;   // -EEXIST
			if (!m_it[i].used && free < 0) free = i;
		}
		if (free < 0) return -28;   // -ENOSPC
		m_it[free] = { true, fd, events, data };
		return 0;
	}

	// EPOLL_CTL_MOD: change an existing fd's events/data. -ENOENT if not registered.
	int mod(int fd, unsigned events, unsigned long long data) {
		SpinGuard g(m_lock);
		for (int i = 0; i < MAXI; i++)
			if (m_it[i].used && m_it[i].fd == fd) { m_it[i].events = events; m_it[i].data = data; return 0; }
		return -2;   // -ENOENT
	}

	// EPOLL_CTL_DEL: drop an fd. -ENOENT if not registered.
	int del(int fd) {
		SpinGuard g(m_lock);
		for (int i = 0; i < MAXI; i++)
			if (m_it[i].used && m_it[i].fd == fd) { m_it[i].used = false; return 0; }
		return -2;   // -ENOENT
	}

	// Snapshot the current interest set into caller-provided storage (so the readiness scan runs
	// without holding m_lock, avoiding a lock-order tangle with the fd/pipe/socket locks). Returns
	// the count copied.
	int snapshot(Interest* out, int cap) {
		SpinGuard g(m_lock);
		int n = 0;
		for (int i = 0; i < MAXI && n < cap; i++)
			if (m_it[i].used) out[n++] = m_it[i];
		return n;
	}

private:
	Interest m_it[MAXI];
	int m_refs;
	mutable Spinlock m_lock;
};

}  // namespace kernel
