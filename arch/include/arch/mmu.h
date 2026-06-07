/*
 * arch/mmu.h — MI/MD contract for the MMU.
 *
 * The page-table format is arch-specific (x86: two-level PDE/PTE + CR3). MI code
 * uses arch-neutral page flags and asks the arch to bring up kernel paging from
 * the physical frame allocator. Per-process address spaces are declared here for
 * the upcoming ring-3/scheduler/fork work; only mmuInitKernel is defined today.
 */
#pragma once
#include <stdint.h>

namespace kernel { class FrameAllocator; }

namespace arch {

enum PageFlags { PAGE_PRESENT = 1, PAGE_WRITE = 2, PAGE_USER = 4 };

// Build the kernel page tables (identity-map all RAM), reserve the arch windows,
// load the directory and enable paging.
void mmuInitKernel(kernel::FrameAllocator& fa, uint32_t topOfRam);

// --- forward-looking: per-process address spaces (defined with the ring-3 work) ---
struct AddressSpace;
AddressSpace* mmuCreateAddressSpace();
void mmuMap(AddressSpace*, uint32_t va, uint32_t pa, uint32_t flags);
void mmuUnmap(AddressSpace*, uint32_t va);
void mmuSwitch(AddressSpace*);
void mmuDestroyAddressSpace(AddressSpace*);

}
