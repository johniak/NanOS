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
 * coalescing with both neighbours on free. All internal links remain 32-bit OFFSETS
 * from the arena base (not raw pointers) — kernel arenas never exceed 4 GiB, so this is
 * a deliberate, documented bound; the same code is correct on the 32-bit kernel and the
 * 64-bit host test harness. The PUBLIC size API is size_t-wide so an over-large request
 * is rejected (OOM) rather than silently truncated. Payloads are 8-byte aligned.
 *
 * Pure logic over a caller-supplied arena -> host-testable (tests/test_memory_manager).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Spinlock.h"   // SMP: one lock serializes the free list + coalescing

namespace kernel {

class Heap {
public:
	// Lay the arena out as one big free block. base/size need not be aligned; the heap
	// uses the 8-aligned sub-range. Safe to call once before any alloc.
	void init(void* base, size_t size);

	void* alloc(size_t size);              // 0 on out-of-memory
	void free(void* ptr);                  // ignores 0 and double-frees
	void* realloc(void* ptr, size_t size);

	size_t freeBytes() const;              // total free payload bytes (for tests/stats)
	// SMP: malloc/free/realloc lock m_lock (a non-recursive spinlock) at the public entry and
	// run the *Locked() bodies; realloc reuses the unlocked bodies so it never re-takes the lock.
	size_t totalBytes() const { return m_end; }   // arena size (for /proc/meminfo)

	// Heap integrity: each block carries a footer mirroring its header (size|used), so a write
	// that runs past a block — or into a freed block's free-list links — desyncs them. We check
	// the boundary tags on every alloc/free; on a mismatch we call this hook (the kernel wires it
	// to a panic that prints the block + halts) instead of corrupting on, turning a silent
	// heap-smash into a clean, located failure. Null (host tests) = the check is a no-op.
	typedef void (*CorruptFn)(const char* what, uintptr_t off, uintptr_t hdr, uintptr_t ftr);
	static void onCorruption(CorruptFn fn) { s_corrupt = fn; }

private:
	char*    m_base;       // 8-aligned arena start (block offset 0 lives here)
	unsigned m_end;        // offset one past the last usable byte (blocks tile [0,m_end))
	unsigned m_freeHead;   // offset of the first free block, or NIL
	mutable Spinlock m_lock;   // SMP: serializes all free-list mutation + reads

	// Unlocked bodies — the caller already holds m_lock (so realloc can compose alloc+free).
	void* allocLocked(size_t size);
	void  freeLocked(void* ptr);

	static CorruptFn s_corrupt;
	// Validate a block's boundary tags + range; reports via s_corrupt and returns false if bad.
	bool      checkBlock(unsigned off) const;
	void      layCanary(unsigned off, unsigned userSize);   // record size + red-zone on alloc
	bool      checkCanary(unsigned off) const;              // verify red-zone on free/realloc

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
