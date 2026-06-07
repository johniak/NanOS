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

// Physical address of the kernel page directory (for building per-process
// directories that share the kernel half, and for switching back on exit).
uint32_t mmuKernelDirPhys();

// Read/load the active page-directory physical address (CR3 on x86). MI code uses
// these to stage a spawned child under the kernel identity map, then restore the
// caller's space — without depending on the x86 control-register details.
uint32_t mmuCurrentDirPhys();
void mmuLoadDirPhys(uint32_t dirPhys);

// Per-process address spaces. mmuCreateAddressSpace returns a space that shares
// the kernel half and has a private (initially empty) user window.
struct AddressSpace;
AddressSpace* mmuCreateAddressSpace();
void mmuMap(AddressSpace*, uint32_t va, uint32_t pa, uint32_t flags);
void mmuUnmap(AddressSpace*, uint32_t va);
void mmuSwitch(AddressSpace*);
void mmuDestroyAddressSpace(AddressSpace*);
// Tear down a process address space: free the user-window frames + the private user
// page table + the directory (NOT the shared kernel-half page tables).
void mmuFreeAddressSpace(AddressSpace*);
// Eager fork copy: a new space sharing the kernel half, with the user window copied
// frame-by-frame from `src`.
AddressSpace* mmuCopyAddressSpace(AddressSpace* src);

}
