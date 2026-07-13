/*
 * Shm.h — a page-frame-backed anonymous shared-memory object: the kernel backing for
 * memfd_create(2).
 *
 * Unlike a RamFs file (contents in one malloc'd buffer, whose mmap is a PRIVATE copy), an Shm owns a
 * set of physical PAGE FRAMES. Those exact frames are mapped into every process that MAP_SHARED-maps
 * the fd, so writes are mutually visible — real cross-process shared memory (Chromium/Electron build
 * their Mojo shared buffers on memfd). read()/write()/ftruncate() all act on the SAME frames, so an
 * Shm has a single source of truth (no data/frame divergence).
 *
 * Frame ownership: the frames are freed only when the last fd referencing the Shm is closed
 * (refcounted like a pipe end — memfd_create opens one ref, dup/fork bump, close drops). A user
 * mapping does NOT itself pin the object, so a process must keep the fd open for as long as it uses
 * the mapping (exactly how Chromium holds its memfd). The mmap teardown path (PTE_SHARED) drops the
 * page-table entries without freeing the frames, so one process exiting never pulls the memory out
 * from under another.
 *
 * The physical-frame backend is injected (ShmFrames) so this stays a pure, host-testable data
 * structure: the kernel plugs in the real frame allocator; the doctest host build plugs in malloc.
 */
#pragma once

#include "string.h"          // memcpy/memset (freestanding)
#include "memory_manager.h"  // malloc/realloc/free — the kernel-heap decls (host build backs them with
                             // libc malloc at link). NEVER <cstdlib> here: mixing the two in one TU
                             // clashes (C++ vs C linkage / noexcept), and Syscall.cpp already has this.
#include "Spinlock.h"

namespace kernel {

// Physical-frame backend. alloc() must return a PAGE-aligned, ZEROED page whose returned value is
// usable directly as an address (an identity-mapped physical frame in the kernel; a malloc'd block
// in host tests) — 0 on out-of-memory. free() releases one such page.
struct ShmFrames {
	unsigned long (*alloc)();
	void (*release)(unsigned long frame);
};

class Shm {
public:
	static const unsigned PAGE = 4096;

	explicit Shm(const ShmFrames& backend)
		: m_fr(backend), m_frames(0), m_npages(0), m_cap(0), m_size(0), m_refs(1) {}

	~Shm() {
		for (unsigned i = 0; i < m_npages; i++)
			m_fr.release(m_frames[i]);
		free(m_frames);
	}

	void ref()   { SpinGuard g(m_lock); m_refs++; }
	// Drop one ref; returns true when the last ref went away (caller deletes).
	bool unref() { SpinGuard g(m_lock); return --m_refs == 0; }

	unsigned long length() const { SpinGuard g(m_lock); return m_size; }
	unsigned pages() const       { SpinGuard g(m_lock); return m_npages; }

	// ftruncate(len): grow to ceil(len/PAGE) frames (freshly allocated ones are zeroed by the
	// backend) or shrink, freeing frames beyond the new tail. Sets the logical size. Returns 0, or
	// -1 on out-of-memory (any frames allocated before the failure are kept — the object is still
	// usable, just smaller than requested, and a retry can extend it).
	int setSize(unsigned long len) {
		SpinGuard g(m_lock);
		unsigned want = (unsigned) ((len + PAGE - 1) / PAGE);
		if (want > m_npages) {
			if (!reserve(want)) return -1;
			while (m_npages < want) {
				unsigned long f = m_fr.alloc();
				if (!f) return -1;
				m_frames[m_npages++] = f;
			}
		} else if (want < m_npages) {
			while (m_npages > want)
				m_fr.release(m_frames[--m_npages]);
		}
		m_size = len;
		return 0;
	}

	// Physical frame backing page `index`, or 0 if beyond the allocated tail. Used by mmap to map
	// the shared frames into a process VA.
	unsigned long frameAt(unsigned index) const {
		SpinGuard g(m_lock);
		return index < m_npages ? m_frames[index] : 0;
	}

	// Byte read at the file position — clamped to the logical size. Returns bytes moved.
	unsigned readAt(unsigned long off, void* buf, unsigned n) const {
		SpinGuard g(m_lock);
		if (off >= m_size) return 0;
		if ((unsigned long) n > m_size - off) n = (unsigned) (m_size - off);
		return copy((char*) buf, off, n, /*toBuf=*/true);
	}

	// Byte write at the file position. A write never allocates new frames (like Linux, a memfd is
	// grown by ftruncate, not by writing past the end): it is clamped to the frames already
	// allocated, and extends the logical size within them. Returns bytes moved.
	unsigned writeAt(unsigned long off, const void* buf, unsigned n) {
		SpinGuard g(m_lock);
		unsigned long capBytes = (unsigned long) m_npages * PAGE;
		if (off >= capBytes) return 0;
		if ((unsigned long) n > capBytes - off) n = (unsigned) (capBytes - off);
		unsigned w = copy((char*) buf, off, n, /*toBuf=*/false);
		if (off + w > m_size) m_size = off + w;
		return w;
	}

private:
	// Ensure the frame-pointer array holds at least `want` slots (geometric growth).
	bool reserve(unsigned want) {
		if (want <= m_cap) return true;
		unsigned nc = m_cap ? m_cap * 2 : 8;
		while (nc < want) nc *= 2;
		unsigned long* p = (unsigned long*) realloc(m_frames, nc * sizeof(unsigned long));
		if (!p) return false;
		m_frames = p; m_cap = nc;
		return true;
	}

	// Move n bytes between `buf` and the frames starting at byte offset `off` (which must lie within
	// [0, m_npages*PAGE)). Splits across page boundaries. `buf` is const on the write path — the cast
	// is safe because toBuf==false only ever reads from it.
	unsigned copy(char* buf, unsigned long off, unsigned n, bool toBuf) const {
		unsigned done = 0;
		while (done < n) {
			unsigned pi = (unsigned) ((off + done) / PAGE);
			unsigned po = (unsigned) ((off + done) % PAGE);
			unsigned k = PAGE - po;
			if (k > n - done) k = n - done;
			char* page = (char*) m_frames[pi];   // frame address (identity phys / host ptr)
			if (toBuf) memcpy(buf + done, page + po, k);
			else       memcpy(page + po, buf + done, k);
			done += k;
		}
		return done;
	}

	const ShmFrames m_fr;
	unsigned long*  m_frames;   // per-page frame addresses
	unsigned        m_npages;   // frames currently allocated
	unsigned        m_cap;      // slots in m_frames[]
	unsigned long   m_size;     // logical size in bytes (ftruncate) — always <= m_npages*PAGE
	int             m_refs;
	mutable Spinlock m_lock;
};

}  // namespace kernel
