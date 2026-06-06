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

	// Physical|offset for a mapped VA, or 0xFFFFFFFF if not mapped.
	uint32_t translate(uint32_t va) const;
	uint32_t directoryPhys() const { return m_dirPhys; }

private:
	PagingEnv m_env;
	uint32_t m_dirPhys;

	uint32_t* dir() const;
};

}
