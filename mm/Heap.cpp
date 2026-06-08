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

unsigned roundUp8(unsigned x) { return (x + 7u) & ~7u; }
}

void Heap::setBlk(unsigned off, unsigned size, unsigned used) {
	*word(off) = size | used;                 // header
	*word(off + size - FTR) = size | used;     // footer (mirror, for backward coalescing)
}

void Heap::flInsert(unsigned off) {
	setFlPrev(off, NIL);
	setFlNext(off, m_freeHead);
	if (m_freeHead != NIL)
		setFlPrev(m_freeHead, off);
	m_freeHead = off;
}

void Heap::flRemove(unsigned off) {
	unsigned p = flPrev(off), n = flNext(off);
	if (p != NIL) setFlNext(p, n); else m_freeHead = n;
	if (n != NIL) setFlPrev(n, p);
}

void Heap::init(void* base, unsigned size) {
	uintptr_t raw = (uintptr_t) base;
	uintptr_t start = (raw + 7u) & ~(uintptr_t) 7u;   // 8-align the arena start
	unsigned lost = (unsigned) (start - raw);
	unsigned usable = (size > lost) ? (size - lost) : 0;
	usable &= ~7u;                                     // whole 8-byte units
	m_base = (char*) start;
	m_end = usable;
	m_freeHead = NIL;
	if (usable >= MIN_BLOCK) {
		setBlk(0, usable, 0);                          // one big free block
		flInsert(0);
	}
}

void* Heap::alloc(unsigned size) {
	if (size == 0)
		size = 1;
	unsigned need = roundUp8(size);
	if (need < MIN_PAYLOAD)
		need = MIN_PAYLOAD;
	unsigned want = need + OVERHEAD;                   // total block bytes required

	// First-fit over the free list.
	unsigned o = m_freeHead;
	while (o != NIL && blkSize(o) < want)
		o = flNext(o);
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
	return m_base + o + HDR;
}

void Heap::free(void* ptr) {
	if (!ptr)
		return;
	unsigned o = (unsigned) ((char*) ptr - m_base - HDR);
	if (!blkUsed(o))
		return;                                        // double-free / not ours: ignore
	unsigned size = blkSize(o);

	// Coalesce forward.
	unsigned next = o + size;
	if (next < m_end && !blkUsed(next)) {
		flRemove(next);
		size += blkSize(next);
	}
	// Coalesce backward (read the previous block's footer).
	if (o > 0) {
		unsigned prevSize = *word(o - FTR) & ~7u;
		unsigned prev = o - prevSize;
		if (!blkUsed(prev)) {
			flRemove(prev);
			o = prev;
			size += prevSize;
		}
	}
	setBlk(o, size, 0);
	flInsert(o);
}

void* Heap::realloc(void* ptr, unsigned size) {
	if (!ptr)
		return alloc(size);
	if (size == 0) {
		free(ptr);
		return 0;
	}
	unsigned o = (unsigned) ((char*) ptr - m_base - HDR);
	unsigned cap = blkSize(o) - OVERHEAD;              // current payload capacity
	if (size <= cap)
		return ptr;                                    // fits in place
	void* np = alloc(size);
	if (!np)
		return 0;
	memcpy(np, ptr, cap);                              // copy the old payload
	free(ptr);
	return np;
}

unsigned Heap::freeBytes() const {
	unsigned total = 0;
	for (unsigned o = m_freeHead; o != NIL; o = flNext(o))
		total += blkSize(o) - OVERHEAD;
	return total;
}

}
