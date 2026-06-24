/*
 * Heap.cpp — implementation of the free-list kernel heap. See Heap.h.
 */
#include "Heap.h"
#include <string.h>

namespace kernel {

namespace {
const unsigned HDR = 8;            // header bytes (size|used in the low word)
const unsigned FTR = 8;            // footer bytes (size|used in its low word)
const unsigned OVERHEAD = HDR + FTR;
const unsigned MIN_PAYLOAD = 8;    // room for the two free-list link offsets
const unsigned MIN_BLOCK = MIN_PAYLOAD + OVERHEAD;   // 24, multiple of 8
const unsigned NIL = 0xFFFFFFFFu;
const unsigned REDZONE = 8;        // canary bytes laid right after each allocation's user data
const unsigned char CANARY = 0xC7; // a write past `size` clobbers it -> caught on free/realloc

unsigned roundUp8(unsigned x) { return (x + 7u) & ~7u; }
}

Heap::CorruptFn Heap::s_corrupt = 0;

// A block is consistent if its offset is in range + 8-aligned, its size is sane, and its footer
// (size|used at off+size-FTR) mirrors its header. A mismatch means a neighbour overran into this
// block's tag, or a use-after-free overwrote a freed block's links — either way, corruption.
bool Heap::checkBlock(unsigned off) const {
	if (off >= m_end || (off & 7u))
		goto bad;
	{
		unsigned hdr = *word(off);
		unsigned size = hdr & ~7u;
		if (size < MIN_BLOCK || off + size > m_end)
			goto bad;
		unsigned ftr = *word(off + size - FTR);
		if (ftr != hdr) {
			if (s_corrupt) s_corrupt("block tag mismatch", off, hdr, ftr);
			return false;
		}
		// For a FREE block the payload holds the two free-list links (off+8, off+12). A
		// use-after-free that scribbles them would send flRemove's pointer surgery into wild
		// memory, so validate they are NIL or a real in-range, aligned block offset.
		if ((hdr & 1u) == 0) {
			unsigned n = *word(off + 8), p = *word(off + 12);
			if ((n != NIL && (n >= m_end || (n & 7u))) ||
			    (p != NIL && (p >= m_end || (p & 7u)))) {
				if (s_corrupt) s_corrupt("free-list link smashed", off, n, p);
				return false;
			}
		}
		return true;
	}
bad:
	if (s_corrupt) s_corrupt("block out of range", off, off < m_end ? *word(off) : 0, m_end);
	return false;
}

void Heap::setBlk(unsigned off, unsigned size, unsigned used) {
	*word(off) = size | used;                 // header
	*word(off + size - FTR) = size | used;     // footer (mirror, for backward coalescing)
}

void Heap::flInsert(unsigned off) {
	setFlPrev(off, NIL);
	setFlNext(off, m_freeHead);
	if (m_freeHead != NIL) {
		if (m_freeHead >= m_end || (m_freeHead & 7u)) {   // freehead smashed by an overflow?
			if (s_corrupt) s_corrupt("free-list head smashed", off, m_freeHead, m_end);
			return;
		}
		setFlPrev(m_freeHead, off);
	}
	m_freeHead = off;
}

void Heap::flRemove(unsigned off) {
	unsigned p = flPrev(off), n = flNext(off);
	if (p != NIL) setFlNext(p, n); else m_freeHead = n;
	if (n != NIL) setFlPrev(n, p);
}

void Heap::init(void* base, size_t size) {
	uintptr_t raw = (uintptr_t) base;
	uintptr_t start = (raw + 7u) & ~(uintptr_t) 7u;   // 8-align the arena start
	unsigned lost = (unsigned) (start - raw);
	size_t avail = (size > lost) ? (size - lost) : 0;
	if (avail > 0xFFFFFFF8u)                            // arena offsets are 32-bit (see Heap.h): cap
		avail = 0xFFFFFFF8u;
	unsigned usable = (unsigned) avail;
	usable &= ~7u;                                     // whole 8-byte units
	m_base = (char*) start;
	m_end = usable;
	m_freeHead = NIL;
	if (usable >= MIN_BLOCK) {
		setBlk(0, usable, 0);                          // one big free block
		flInsert(0);
	}
}

// Stash the user-requested size in the header's unused high word (free blocks don't use it),
// and lay a CANARY run right after the user's `size` bytes. checkCanary() then verifies it on
// free/realloc, pinpointing the allocation that was written past its end.
void Heap::layCanary(unsigned off, unsigned userSize) {
	*word(off + 4) = userSize;
	memset(m_base + off + HDR + userSize, CANARY, REDZONE);
}
bool Heap::checkCanary(unsigned off) const {
	unsigned userSize = *word(off + 4);
	const unsigned char* z = (const unsigned char*) (m_base + off + HDR + userSize);
	if (userSize > blkSize(off)) {                     // size field itself trashed
		if (s_corrupt) s_corrupt("alloc size field smashed", off, userSize, blkSize(off));
		return false;
	}
	for (unsigned i = 0; i < REDZONE; i++)
		if (z[i] != CANARY) {
			if (s_corrupt) s_corrupt("buffer overflow past allocation", off, userSize, z[i]);
			return false;
		}
	return true;
}

void* Heap::alloc(size_t size) {
	SpinIrqGuard g(m_lock);
	return allocLocked(size);
}

void* Heap::allocLocked(size_t size) {
	unsigned size32 = (unsigned) size;            // arena offsets are 32-bit by design (see Heap.h)
	if ((size_t) size32 != size)
		return 0;                                 // request too large for a 32-bit-offset arena -> OOM
	if (size32 == 0)
		size32 = 1;
	unsigned userSize = size32;
	unsigned need = roundUp8(userSize + REDZONE);      // room for the user data + its red-zone
	if (need < MIN_PAYLOAD)
		need = MIN_PAYLOAD;
	unsigned want = need + OVERHEAD;                   // total block bytes required

	// First-fit over the free list, validating each block BEFORE reading its size (a corrupted
	// link or boundary tag is caught here rather than dereferenced into wild memory).
	unsigned o = m_freeHead;
	while (o != NIL) {
		if (!checkBlock(o)) return 0;                  // validate first, then it is safe to read
		if (blkSize(o) >= want) break;
		o = flNext(o);
	}
	if (o == NIL)
		return 0;                                      // out of memory

	unsigned have = blkSize(o);
	flRemove(o);
	// Split off the remainder if it is big enough to be its own block.
	if (have - want >= MIN_BLOCK) {
		setBlk(o, want, 1);
		unsigned rest = o + want;
		setBlk(rest, have - want, 0);
		flInsert(rest);
	} else {
		setBlk(o, have, 1);
	}
	layCanary(o, userSize);
	return m_base + o + HDR;
}

void Heap::free(void* ptr) {
	SpinIrqGuard g(m_lock);
	freeLocked(ptr);
}

void Heap::freeLocked(void* ptr) {
	if (!ptr)
		return;
	unsigned o = (unsigned) ((char*) ptr - m_base - HDR);
	if (!blkUsed(o))
		return;                                        // double-free / not ours: ignore
	if (!checkBlock(o))                                 // the block being freed is intact?
		return;
	checkCanary(o);                                    // ...and nobody wrote past its end
	unsigned size = blkSize(o);

	// Coalesce forward.
	unsigned next = o + size;
	if (next < m_end && !blkUsed(next)) {
		if (!checkBlock(next)) return;
		flRemove(next);
		size += blkSize(next);
	}
	// Coalesce backward (read the previous block's footer).
	if (o > 0) {
		unsigned prevSize = *word(o - FTR) & ~7u;
		unsigned prev = o - prevSize;
		if (prev < o && !blkUsed(prev) && checkBlock(prev)) {
			flRemove(prev);
			o = prev;
			size += prevSize;
		}
	}
	setBlk(o, size, 0);
	flInsert(o);
}

void* Heap::realloc(void* ptr, size_t size) {
	SpinIrqGuard g(m_lock);   // held across the alloc+copy+free below (all use the *Locked bodies)
	if (!ptr)
		return allocLocked(size);
	if (size == 0) {
		freeLocked(ptr);
		return 0;
	}
	unsigned size32 = (unsigned) size;                 // arena offsets are 32-bit by design (see Heap.h)
	if ((size_t) size32 != size)
		return 0;                                      // request too large -> OOM
	unsigned o = (unsigned) ((char*) ptr - m_base - HDR);
	if (!checkBlock(o))
		return 0;
	checkCanary(o);
	unsigned oldSize = *word(o + 4);
	unsigned cap = blkSize(o) - OVERHEAD - REDZONE;    // usable bytes (the red-zone is excluded)
	if (size32 <= cap) {                               // fits in place: move the canary to the new end
		layCanary(o, size32);
		return ptr;
	}
	void* np = allocLocked(size);
	if (!np)
		return 0;
	memcpy(np, ptr, oldSize < size32 ? oldSize : size32);  // preserve min(old,new) user bytes
	freeLocked(ptr);
	return np;
}

size_t Heap::freeBytes() const {
	SpinIrqGuard g(m_lock);
	size_t total = 0;
	for (unsigned o = m_freeHead; o != NIL; o = flNext(o))
		total += blkSize(o) - OVERHEAD;
	return total;
}

}
