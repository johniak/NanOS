/*
 * AddressSpace.h
 *
 * A single x86 page directory plus on-demand page tables — map/unmap/translate
 * at 4 KiB granularity, and the physical address to load into CR3. The whole
 * thing is host-testable because it never dereferences a physical address
 * directly: an injected PagingEnv supplies frame alloc/free and a
 * physical->editable-pointer translation. In the kernel that translation is
 * identity (we identity-map all RAM); in tests it is arena-relative.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct PagingEnv {
	uint32_t (*allocFrame)(void* ctx);          // phys addr of a free frame, 0 = OOM
	void (*freeFrame)(void* ctx, uint32_t pa);
	void* (*physToVirt)(void* ctx, uint32_t pa); // editable pointer for a phys addr
	void* ctx;
};

class AddressSpace {
public:
	explicit AddressSpace(const PagingEnv& env);   // allocates + zeroes the directory

	bool map(uint32_t va, uint32_t pa, uint32_t flags);   // alloc PT on demand
	void unmap(uint32_t va);                              // clear the PTE (keep the PT)
	bool mapRange(uint32_t va, uint32_t pa, uint32_t len, uint32_t flags);

	// Copy all 1024 page-directory entries from another directory (the kernel's)
	// into this one — sharing every kernel page table — then clear the PDE
	// covering `userVa` so a later map() of that region allocates a fresh PRIVATE
	// page table instead of mutating the shared kernel one. Builds a per-process
	// space: kernel half shared (supervisor), user window private.
	void adoptKernelDirectory(uint32_t kernelDirPhys, uint32_t userVa);

	// Free the page table covering `userVa` and every present frame it maps, then
	// clear that PDE. Used to tear down a process's private user window WITHOUT
	// touching the shared kernel-half page tables (which other PDEs alias).
	void freeUserWindow(uint32_t userVa);

	// Eager fork copy: for every present page in `src`'s user-window PT (the PDE
	// covering `userVa`), allocate a fresh frame, copy the bytes, and map it here
	// with the same flags. This space must already share the kernel half and have
	// a private (empty) user window (adoptKernelDirectory).
	void copyUserWindowFrom(const AddressSpace& src, uint32_t userVa);

	// Physical|offset for a mapped VA, or 0xFFFFFFFF if not mapped.
	uint32_t translate(uint32_t va) const;
	uint32_t directoryPhys() const { return m_dirPhys; }

private:
	PagingEnv m_env;
	uint32_t m_dirPhys;

	uint32_t* dir() const;
};

}
