/*
 * Heap.h — a real kernel byte-heap allocator (free list + coalescing), the engine
 * behind malloc/free/realloc (memory_manager.cpp wraps a global instance).
 *
 * Replaces the old bump allocator whose free() was a no-op: this one reclaims memory
 * and merges adjacent free blocks, so a workload that allocates and frees a lot (e.g.
 * Doom's per-read path resolution) no longer grows the heap without bound.
 *
 * Design: boundary-tag blocks (8-byte header + 8-byte footer, both holding size|used)
 * with an explicit doubly-linked free list (first-fit), splitting on alloc and
 * coalescing with both neighbours on free. All internal links are 32-bit OFFSETS from
 * the arena base (not raw pointers), so the same code is correct on the 32-bit kernel
 * and the 64-bit host test harness. Payloads are 8-byte aligned.
 *
 * Pure logic over a caller-supplied arena -> host-testable (tests/test_memory_manager).
 */
#pragma once
#include <stdint.h>

namespace kernel {

class Heap {
public:
	// Lay the arena out as one big free block. base/size need not be aligned; the heap
	// uses the 8-aligned sub-range. Safe to call once before any alloc.
	void init(void* base, unsigned size);

	void* alloc(unsigned size);            // 0 on out-of-memory
	void free(void* ptr);                  // ignores 0 and double-frees
	void* realloc(void* ptr, unsigned size);

	unsigned freeBytes() const;            // total free payload bytes (for tests/stats)
	unsigned totalBytes() const { return m_end; }   // arena size (for /proc/meminfo)

private:
	char*    m_base;       // 8-aligned arena start (block offset 0 lives here)
	unsigned m_end;        // offset one past the last usable byte (blocks tile [0,m_end))
	unsigned m_freeHead;   // offset of the first free block, or NIL

	uint32_t* word(unsigned off) const { return (uint32_t*) (m_base + off); }
	unsigned  blkSize(unsigned off) const { return *word(off) & ~7u; }
	bool      blkUsed(unsigned off) const { return (*word(off) & 1u) != 0; }
	void      setBlk(unsigned off, unsigned size, unsigned used);

	// Free-list links live in the payload of a free block (two offsets).
	unsigned  flNext(unsigned off) const { return *word(off + 8); }
	unsigned  flPrev(unsigned off) const { return *word(off + 12); }
	void      setFlNext(unsigned off, unsigned v) { *word(off + 8) = v; }
	void      setFlPrev(unsigned off, unsigned v) { *word(off + 12) = v; }
	void      flInsert(unsigned off);
	void      flRemove(unsigned off);
};

}
