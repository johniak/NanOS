/*
 * AddressSpace.h
 *
 * A single x86_64 PML4 plus on-demand lower tables (PDPT/PD/PT) — map/unmap/translate at
 * 4 KiB granularity, and the physical address to load into CR3. Host-testable: it never
 * dereferences a physical address directly. An injected PagingEnv supplies frame
 * alloc/free and a physical->editable-pointer translation (identity in the kernel,
 * arena-relative in tests). Public method names mirror the i686 AddressSpace so MI
 * callers are unchanged; only the address types widen to uint64_t.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct PagingEnv {
	uint64_t (*allocFrame)(void* ctx);            // phys addr of a free frame, 0 = OOM
	void (*freeFrame)(void* ctx, uint64_t pa);
	void* (*physToVirt)(void* ctx, uint64_t pa);  // editable pointer for a phys addr
	void* ctx;
};

class AddressSpace {
public:
	explicit AddressSpace(const PagingEnv& env);   // allocates + zeroes the PML4

	bool map(uint64_t va, uint64_t pa, uint64_t flags);   // alloc intermediate tables on demand
	void unmap(uint64_t va);                              // clear the PTE (keep the tables)
	bool mapRange(uint64_t va, uint64_t pa, uint64_t len, uint64_t flags);

	// Copy all 512 PML4 entries from another space (the kernel's) into this one — sharing
	// every kernel lower table — then privatize the path to `userVa` (a private PML4->PDPT->PD
	// chain) and clear that PD entry, so a later map() of that region allocates a fresh PRIVATE
	// page table instead of mutating a shared kernel one. Builds a per-process space: kernel
	// mappings shared (by value), user window private.
	void adoptKernelDirectory(uint64_t kernelTopPhys, uint64_t userVa);

	// Privatize the PML4->PDPT->PD path to `va` and clear the PD entry covering it (no frames
	// freed), so a later map() of that region allocates a fresh PRIVATE page table. Extends the
	// private user window beyond the single PD entry dropped by adoptKernelDirectory.
	void dropPde(uint64_t va);

	// Free the page table covering `userVa` and every present frame it maps, then clear that
	// PD entry. Tears down a process's private user window WITHOUT touching shared kernel tables.
	void freeUserWindow(uint64_t userVa);

	// Eager fork copy: for every present page in `src`'s user-window PT (the PD entry covering
	// `userVa`), allocate a fresh frame, copy the bytes, and map it here with the same flags.
	// This space must already share the kernel half and have a private (empty) user window.
	// Returns false if a frame/table allocation failed partway (the caller tears down).
	bool copyUserWindowFrom(const AddressSpace& src, uint64_t userVa);

	// Physical|offset for a mapped VA, or 0xFFFFFFFFFFFFFFFF if not mapped.
	uint64_t translate(uint64_t va) const;
	uint64_t directoryPhys() const { return m_topPhys; }   // PML4 phys (the CR3 value)

private:
	PagingEnv m_env;
	uint64_t m_topPhys;                            // PML4 physical address

	uint64_t* top() const;
	// Return the next-level table for `parent[idx]`, allocating+linking it if absent (when
	// create). Returns nullptr on OOM or (when !create) a missing entry.
	uint64_t* nextTable(uint64_t* parent, uint64_t idx, bool create);
	// Walk PML4->PT, returning a pointer to the PTE slot for `va`. Allocates intermediate
	// tables when create=true. Returns nullptr on OOM / a missing level (when !create).
	uint64_t* walk(uint64_t va, bool create);
	// Replace parent[idx]'s table with a PRIVATE copy (alloc, copy 512 entries, relink).
	// Returns the private table pointer, or nullptr on OOM / absent entry.
	uint64_t* privatizeChild(uint64_t* parent, uint64_t idx);
	// Give this space a private PML4->PDPT->PD chain along `va`; returns the private PD.
	uint64_t* privatizePdPath(uint64_t va);
};

}
