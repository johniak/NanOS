/*
 * MultibootMmap.h
 *
 * Pure helpers over the Multiboot memory map: iterate the entries and compute
 * the highest usable physical address. No hardware access — host-testable.
 *
 * The buffer-based functions (parseMmapBuffer / highestUsableInBuffer) take a
 * real pointer + byte length and hold all the logic, so they are testable on a
 * 64-bit host. The mbi-based wrappers are thin conveniences for the kernel,
 * where mmap_addr is a genuine 32-bit physical (identity-mapped) address.
 */
#pragma once
#include "MultibootInfo.h"

namespace kernel {

typedef void (*MmapCallback)(void* ctx, uint64_t base, uint64_t length, uint32_t type);

// Walk a raw mmap buffer of `len` bytes, invoking cb for each entry. The next
// entry sits at (current + entry.size + 4) — `size` excludes itself.
void parseMmapBuffer(const void* mmap, uint32_t len, void* ctx, MmapCallback cb);

// Highest usable physical byte (one past the top of the highest available
// region) found in a raw mmap buffer; 0 if none. Not clamped (returns u64).
uint64_t highestUsableInBuffer(const void* mmap, uint32_t len);

// Convenience: iterate the map referenced by `mbi` (no-op without the flag).
void parseMmap(const MultibootInfo* mbi, void* ctx, MmapCallback cb);

// Highest usable physical byte (64-bit), capped at the frame-pool capacity (16 GiB). Uses the
// mmap when present, else the mem_upper fallback, else 0.
uint64_t highestUsableAddr(const MultibootInfo* mbi);

}
