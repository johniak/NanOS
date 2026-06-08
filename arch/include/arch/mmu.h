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

// Identity-map a physical MMIO span (e.g. the framebuffer LFB, which sits above
// RAM and is therefore NOT covered by the kernel identity map) into the kernel
// directory, present+writable, supervisor. Call after mmuInitKernel and BEFORE any
// per-process address space is created, so the new PDE is shared by every process.
void mmuMapKernelMmio(uint32_t phys, uint32_t bytes);

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
// Physical address of a space's page directory (the CR3 value for entering it).
uint32_t mmuSpaceDirPhys(AddressSpace*);

// Map a framebuffer's physical region into a process address space at a fixed user VA
// (above RAM, separate from the 1 MiB user window), present+writable+user. Returns the
// user virtual address of the framebuffer, or 0 on failure. Used by mmap of /dev/fb0.
uint32_t mmuMapUserFb(AddressSpace*, uint32_t fbPhys, uint32_t bytes);

}
