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

// VA window layout — the SINGLE source of truth for both the MD mmu impl and MI staging
// (Exec). All bases are low (< 2 GiB) so bit 47 = 0 and they are canonical by construction
// (spec §1/§8). Started from the i686 offsets to keep the x86_64 port minimal; the full
// 128 TiB user split is deferred. On i686 these same values fit uint32_t — the constants are
// arch-neutral, only the page-table format differs.
constexpr uint64_t VA_USER_BASE     = 0x800000;     // program image + user stack
// Window enlarged 16 MiB -> 64 MiB (x86_64 only; i686 keeps its own 8 MiB window in arch/x86).
// A 64-bit .nxe is roughly twice its i686 size, so big apps (e.g. NetSurf, ~16 MiB image+bss)
// overflowed the old 8 MiB window. The band up to VA_MODULE_BASE (1 GiB) is otherwise unused, so
// this is free VA. The image loads at VA_USER_BASE growing up; the user stack sits at the top.
constexpr uint64_t VA_USER_END      = 0x4000000;    // exclusive (64 MiB): window = [0x800000, 0x4000000)
constexpr uint64_t VA_MODULE_BASE   = 0x40000000;   // .ndl load band (1 GiB)
constexpr uint64_t VA_MODULE_STRIDE = 0x00400000;   // per-module spacing (4 MiB)
constexpr uint64_t VA_MODULE_MAX    = 0x48000000;   // +128 MiB (32 modules)
constexpr uint64_t VA_HEAP_BASE     = 0x48000000;   // brk/sbrk anonymous heap
constexpr uint64_t VA_HEAP_MAX      = 0x4C000000;   // +64 MiB
constexpr uint64_t VA_MMAP_BASE     = 0x50000000;   // anonymous/file-backed mmap
constexpr uint64_t VA_MMAP_MAX      = 0x54000000;   // +64 MiB
constexpr uint64_t VA_FB_BASE       = 0x58000000;   // user framebuffer window (1.375 GiB)
constexpr uint64_t VA_FB_MAX        = VA_FB_BASE + 0x4000000;   // +64 MiB (bounds the fb window for enum)

// Build the kernel page tables (identity-map all RAM), reserve the arch windows,
// load the directory and enable paging.
void mmuInitKernel(kernel::FrameAllocator& fa, uint64_t topOfRam);

// Physical address of the kernel page directory (for building per-process
// directories that share the kernel half, and for switching back on exit).
uint32_t mmuKernelDirPhys();

// Identity-map a physical MMIO span (e.g. the framebuffer LFB, which sits above
// RAM and is therefore NOT covered by the kernel identity map) into the kernel
// directory, present+writable, supervisor. Call after mmuInitKernel and BEFORE any
// per-process address space is created, so the new PDE is shared by every process.
// phys is 64-bit: PCI MMIO BARs (framebuffer, NIC, xHCI) on real x86_64 hardware are
// routinely programmed above 4 GiB. Truncating to 32 bits maps the wrong page and the
// first access triple-faults on the metal (QEMU/OVMF places them low, hiding it). The
// i686 impl narrows internally (its physical space is 32-bit anyway).
void mmuMapKernelMmio(uint64_t phys, uint32_t bytes);

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
uint32_t mmuMapUserFb(AddressSpace*, uint64_t fbPhys, uint32_t bytes);   // fbPhys 64-bit: real HW LFB >4 GiB

// Growable anonymous user heap (the brk/sbrk region). It lives at a fixed high VA,
// above RAM and the framebuffer window, so it is independent of the 4 MiB user window.
// mmuUserHeapBase/Max bound it; mmuSetUserBrk grows (maps fresh zeroed USER|RW frames)
// or shrinks (unmaps + frees) the mapping between two break values. Page-granular: the
// break itself can be byte-granular, the mapping rounds to whole pages. Returns 0 on
// success, -1 on out-of-memory. The teardown/fork of these PDEs is handled inside
// mmuFreeAddressSpace / mmuCopyAddressSpace.
uint32_t mmuUserHeapBase();
uint32_t mmuUserHeapMax();
int mmuSetUserBrk(AddressSpace*, uint32_t oldBrk, uint32_t newBrk);

// The shared-library (.ndl) load region: a band of per-module 4 MiB windows above the
// 1 MiB user window and below the framebuffer/heap windows. The dynamic loader assigns
// each loaded module a base here; mmuModuleStride() is the per-module spacing. Teardown
// (mmuFreeAddressSpace) and fork (mmuCopyAddressSpace) cover these PDEs like the heap.
uint32_t mmuModuleBase();
uint32_t mmuModuleMax();
uint32_t mmuModuleStride();

// Anonymous/file-backed mmap window + a primitive to populate it. mmuMapAnon maps `bytes`
// (page-rounded) of fresh zeroed USER frames at `base` (RW when `writable`), for
// mmap(MAP_ANONYMOUS) and as the backing store of a file-backed mmap. The dispatch
// bump-allocates VAs within [mmuMmapBase, mmuMmapMax). Returns 0 on success, -1 on OOM.
uint32_t mmuMmapBase();
uint32_t mmuMmapMax();
int mmuMapAnon(AddressSpace*, uint32_t base, uint32_t bytes, int writable);
// Tear down a mmap'd range: clear the PTE AND return each backing frame to the frame
// allocator, for [base, base+bytes) (page-rounded). The inverse of mmuMapAnon; used by
// SYS_munmap to reclaim physical RAM (the VA reuse is bookkept by the MI free-list). Same
// kernel-CR3 trap as mmuMapAnon — freeing frames touches arbitrary RAM by identity.
void mmuUnmapAnon(AddressSpace*, uint32_t base, uint32_t bytes);

}
