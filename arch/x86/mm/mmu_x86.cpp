/*
 * mmu_x86.cpp — x86 implementation of <arch/mmu.h>.
 *
 * Kernel bring-up (mmuInitKernel: identity-map all RAM, enable paging) plus the
 * per-process address-space API used by the ring-3 program loader. A process
 * directory shares the entire kernel half (so syscalls/IRQs run with the kernel
 * mapped under any CR3) and owns a private page table for the user window.
 */
#include <arch/mmu.h>
#include "AddressSpace.h"
#include "Paging.h"
#include "PagingControl.h"
#include "FrameAllocator.h"
#include "memory_manager.h"   // heapInit
#include <string.h>

// Linker symbol marking the end of the kernel image (arch/x86/linker.ld).
extern char end;

namespace {

// All per-process virtual windows live HIGH in the 32-bit space (>= 1 GiB), well above any RAM we
// identity-map. The kernel reads the REAL RAM size from multiboot (bootMemTop), so it is not tied
// to a fixed amount — but a window placed inside the identity-mapped RAM would force map() to
// mutate a shared kernel page table (and teardown to free kernel-identity frames). Keeping every
// window ABOVE topOfRam keeps its PDE ABSENT in the kernel directory, so map() allocates a PRIVATE
// page table. These used to sit at 128/256/512 MiB, which capped usable RAM at 128 MiB; lifting
// them to the 1 GiB region raises that ceiling to ~1 GiB (enough for the 512 MiB we now run with).

// Per-process user window: program image (bottom) + user stack (top), two 4 MiB PDEs (8 MiB:
// 0x800000..0xFFFFFF) so a large static binary (e.g. the OpenSSL CLI, ~3.7 MiB) fits below the
// stack. Stays LOW (inside the identity range) but its PDEs are dropped to private at creation.
const uint32_t NX_USER_BASE  = 0x800000;
const uint32_t NX_USER_END   = 0x1000000;   // exclusive (16 MiB); window = [0x800000, 0x1000000)

// Shared-library (.ndl) load band: 32 per-module 4 MiB windows. libc.ndl etc. are relocated here
// at load (the loader applies the base delta), so the band can move freely.
const uint32_t NX_MOD_BASE   = 0x40000000;   // 1 GiB
const uint32_t NX_MOD_STRIDE = 0x00400000;
const uint32_t NX_MOD_MAX    = 0x48000000;   // +128 MiB (32 modules)

// Anonymous user heap window (brk/sbrk): up to 64 MiB per process (was 32 MiB).
const uint32_t NX_BRK_BASE = 0x48000000;
const uint32_t NX_BRK_MAX  = NX_BRK_BASE + 64u * 1024u * 1024u;   // -> 0x4C000000

// Anonymous/file-backed mmap window: up to 64 MiB per process.
const uint32_t NX_MMAP_BASE = 0x50000000;
const uint32_t NX_MMAP_MAX  = NX_MMAP_BASE + 64u * 1024u * 1024u; // -> 0x54000000

kernel::FrameAllocator* g_fa = 0;
uint32_t allocFrame(void*) { return g_fa->alloc(); }
void freeFrame(void*, uint32_t pa) { g_fa->free(pa); }
void* physToVirt(void*, uint32_t pa) { return (void*) pa; }  // identity-mapped

kernel::PagingEnv g_env = { allocFrame, freeFrame, physToVirt, 0 };
kernel::AddressSpace* g_kspace = 0;
uint32_t g_kernelDirPhys = 0;

}  // namespace

namespace arch {

// Opaque per-process address space = one kernel::AddressSpace.
struct AddressSpace {
	kernel::AddressSpace impl;
	explicit AddressSpace(const kernel::PagingEnv& e) : impl(e) {}
};

void mmuInitKernel(kernel::FrameAllocator& fa, uint32_t topOfRam) {
	g_fa = &fa;

	// Re-reserve the windows the frame pool must never hand out.
	fa.markRangeUsed(0, 0x100000);                                   // low mem + VGA
	fa.markRangeUsed(0x100000, (uint32_t) (unsigned) &end - 0x100000); // kernel image
	fa.markRangeUsed(0x800000, 0x800000);                           // exec staging window (8 MiB: matches the user window)
	// Kernel byte heap: carve a slice off the TOP of RAM; the rest (below) is the frame pool for
	// user pages. Sized at ~25% of RAM, clamped to [8 MiB, 256 MiB] — so more RAM grows BOTH the
	// heap and the frame pool, instead of a fixed ~123 MiB split that capped the frame pool.
	uint32_t heapSize = topOfRam / 4u;
	if (heapSize > 256u * 1024u * 1024u) heapSize = 256u * 1024u * 1024u;
	if (heapSize < 8u * 1024u * 1024u)   heapSize = 8u * 1024u * 1024u;
	uint32_t heapBase = (topOfRam - heapSize) & kernel::PAGE_MASK;  // kernel byte heap
	fa.markRangeUsed(heapBase, topOfRam - heapBase);
	// Lay out the kernel heap over its reserved region before the first malloc below.
	// Paging is still off here, so the region is directly (identity) accessible.
	heapInit((void*) heapBase, topOfRam - heapBase);

	g_kspace = new kernel::AddressSpace(g_env);
	g_kspace->mapRange(0, 0, topOfRam, kernel::PTE_PRESENT | kernel::PTE_RW);
	g_kernelDirPhys = g_kspace->directoryPhys();

	__asm__ __volatile__("cli");
	kernel::loadCr3(g_kernelDirPhys);
	kernel::enablePaging();
	__asm__ __volatile__("sti");
}

uint32_t mmuKernelDirPhys() { return g_kernelDirPhys; }

void mmuMapKernelMmio(uint32_t phys, uint32_t bytes) {
	uint32_t base = phys & kernel::PAGE_MASK;
	uint32_t end = (phys + bytes + ~kernel::PAGE_MASK) & kernel::PAGE_MASK;  // round up
	// Map into the live kernel directory; these VAs were never touched, so no stale TLB.
	g_kspace->mapRange(base, base, end - base, kernel::PTE_PRESENT | kernel::PTE_RW);
}

uint32_t mmuCurrentDirPhys() { return kernel::readCr3(); }
void mmuLoadDirPhys(uint32_t dirPhys) { kernel::loadCr3(dirPhys); }

AddressSpace* mmuCreateAddressSpace() {
	AddressSpace* s = new AddressSpace(g_env);
	// Share the whole kernel half; the user window (two PDEs) gets private PTs. adopt drops the
	// first PDE; dropPde clears the rest of the window so each map() there allocates a private PT.
	s->impl.adoptKernelDirectory(g_kernelDirPhys, NX_USER_BASE);
	for (uint32_t va = NX_USER_BASE + 0x400000; va < NX_USER_END; va += 0x400000)
		s->impl.dropPde(va);
	return s;
}

void mmuMap(AddressSpace* s, uint32_t va, uint32_t pa, uint32_t flags) {
	s->impl.map(va, pa, flags);
}

void mmuUnmap(AddressSpace* s, uint32_t va) {
	s->impl.unmap(va);
}

void mmuSwitch(AddressSpace* s) {
	kernel::loadCr3(s->impl.directoryPhys());
}

void mmuDestroyAddressSpace(AddressSpace* s) { mmuFreeAddressSpace(s); }

void mmuFreeAddressSpace(AddressSpace* s) {
	if (!s)
		return;
	// Free only the PRIVATE parts: the user-window page table + its frames, the heap
	// PDEs (anonymous RAM, so freeUserWindow's frame-free is correct), and the directory.
	// The kernel-half PDEs alias shared kernel page tables — leave them. The framebuffer
	// window (0x10000000) is MMIO and is intentionally NOT freed here.
	for (uint32_t va = NX_USER_BASE; va < NX_USER_END; va += 0x400000)
		s->impl.freeUserWindow(va);   // the program-image + user-stack PDEs (private)
	for (uint32_t va = NX_BRK_BASE; va < NX_BRK_MAX; va += 0x400000)
		s->impl.freeUserWindow(va);   // no-op for PDEs the heap never grew into
	for (uint32_t va = NX_MOD_BASE; va < NX_MOD_MAX; va += NX_MOD_STRIDE)
		s->impl.freeUserWindow(va);   // shared-library module windows (no-op if unused)
	for (uint32_t va = NX_MMAP_BASE; va < NX_MMAP_MAX; va += 0x400000)
		s->impl.freeUserWindow(va);   // mmap window (no-op for PDEs never mapped)
	g_fa->free(s->impl.directoryPhys());
	delete s;
}

AddressSpace* mmuCopyAddressSpace(AddressSpace* src) {
	AddressSpace* s = new AddressSpace(g_env);
	// Share the kernel half, private (empty) user window, then copy the user pages and
	// every populated heap PDE (fork duplicates the heap, as a Unix child expects).
	s->impl.adoptKernelDirectory(g_kernelDirPhys, NX_USER_BASE);
	for (uint32_t va = NX_USER_BASE + 0x400000; va < NX_USER_END; va += 0x400000)
		s->impl.dropPde(va);
	// Eager copy of every user window. If any allocation fails (we hit the physical-memory
	// ceiling — fork duplicates the program, heap and shared-library pages with no COW), tear
	// the half-built space down and return 0 so fork degrades to -EAGAIN instead of handing
	// back a corrupt child.
	bool ok = true;
	for (uint32_t va = NX_USER_BASE; ok && va < NX_USER_END; va += 0x400000)
		ok = s->impl.copyUserWindowFrom(src->impl, va);   // program-image + user-stack PDEs
	for (uint32_t va = NX_BRK_BASE; ok && va < NX_BRK_MAX; va += 0x400000)
		ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint32_t va = NX_MOD_BASE; ok && va < NX_MOD_MAX; va += NX_MOD_STRIDE)
		ok = s->impl.copyUserWindowFrom(src->impl, va);   // duplicate loaded module windows
	for (uint32_t va = NX_MMAP_BASE; ok && va < NX_MMAP_MAX; va += 0x400000)
		ok = s->impl.copyUserWindowFrom(src->impl, va);   // duplicate mmap'd regions across fork
	if (!ok) {
		mmuFreeAddressSpace(s);
		return 0;
	}
	return s;
}

uint32_t mmuSpaceDirPhys(AddressSpace* s) { return s->impl.directoryPhys(); }

uint32_t mmuMapUserFb(AddressSpace* s, uint32_t fbPhys, uint32_t bytes) {
	const uint32_t FB_USER_VA = 0x58000000;   // 1.375 GiB: above RAM, outside every other window
	uint32_t base = fbPhys & kernel::PAGE_MASK;
	uint32_t off = fbPhys - base;
	uint32_t len = (off + bytes + ~kernel::PAGE_MASK) & kernel::PAGE_MASK;
	// Allocating + zeroing page-table frames touches arbitrary physical RAM by identity,
	// which is only safe under the kernel directory (the process dir's user-window PDE
	// does NOT identity-map all RAM). Switch to the kernel dir for the mapping, then back
	// (the CR3 reload makes the new PTEs live).
	uint32_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	bool ok = s->impl.mapRange(FB_USER_VA, base, len,
			kernel::PTE_PRESENT | kernel::PTE_RW | kernel::PTE_USER);
	kernel::loadCr3(saved);
	return ok ? FB_USER_VA + off : 0;
}

uint32_t mmuUserHeapBase() { return NX_BRK_BASE; }
uint32_t mmuUserHeapMax()  { return NX_BRK_MAX; }

uint32_t mmuModuleBase()   { return NX_MOD_BASE; }
uint32_t mmuModuleMax()    { return NX_MOD_MAX; }
uint32_t mmuModuleStride() { return NX_MOD_STRIDE; }

int mmuSetUserBrk(AddressSpace* s, uint32_t oldBrk, uint32_t newBrk) {
	// Map (grow) or unmap (shrink) whole pages between the two break values. The break is
	// byte-granular but the mapping is page-granular, so round both ends up: the mapped
	// region is always [NX_BRK_BASE, pageUp(brk)).
	uint32_t oldTop = (oldBrk + 0xFFFu) & ~0xFFFu;
	uint32_t newTop = (newBrk + 0xFFFu) & ~0xFFFu;
	// Allocating/zeroing fresh page-table and data frames touches arbitrary RAM by
	// identity, which is only safe under the kernel directory (same trap as mmuMapUserFb).
	uint32_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	int rc = 0;
	if (newTop > oldTop) {
		for (uint32_t va = oldTop; va < newTop; va += 0x1000) {
			uint32_t f = g_fa->alloc();
			if (!f) { rc = -1; break; }
			memset((void*) f, 0, 0x1000);
			if (!s->impl.map(va, f, kernel::PTE_PRESENT | kernel::PTE_RW | kernel::PTE_USER)) {
				g_fa->free(f);
				rc = -1;
				break;
			}
		}
	} else if (newTop < oldTop) {
		for (uint32_t va = newTop; va < oldTop; va += 0x1000) {
			uint32_t pa = s->impl.translate(va);
			s->impl.unmap(va);
			if (pa != 0xFFFFFFFFu)
				g_fa->free(pa);
		}
	}
	kernel::loadCr3(saved);   // CR3 reload flushes the TLB so the new PTEs are live
	return rc;
}

uint32_t mmuMmapBase() { return NX_MMAP_BASE; }
uint32_t mmuMmapMax()  { return NX_MMAP_MAX; }

// Map `bytes` (rounded up to whole pages) of fresh zeroed frames at [base, base+bytes) in
// the process address space, USER and (if writable) RW. Used by mmap(MAP_ANONYMOUS) and as
// the backing store for file-backed mmap. Same kernel-CR3 trap as mmuSetUserBrk: allocating
// and zeroing frames touches arbitrary RAM by identity, only safe under the kernel dir.
int mmuMapAnon(AddressSpace* s, uint32_t base, uint32_t bytes, int writable) {
	uint32_t end = (base + bytes + 0xFFFu) & ~0xFFFu;
	uint32_t flags = kernel::PTE_PRESENT | kernel::PTE_USER | (writable ? kernel::PTE_RW : 0);
	uint32_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	int rc = 0;
	for (uint32_t va = base; va < end; va += 0x1000) {
		uint32_t f = g_fa->alloc();
		if (!f) { rc = -1; break; }
		memset((void*) f, 0, 0x1000);
		if (!s->impl.map(va, f, flags)) {
			g_fa->free(f);
			rc = -1;
			break;
		}
	}
	kernel::loadCr3(saved);
	return rc;
}

// Inverse of mmuMapAnon: clear each PTE in [base, base+bytes) and free the backing frame
// (mirrors the brk-shrink path in mmuSetUserBrk). Same kernel-CR3 trap: freeing frames and
// touching page tables by identity is only safe under the kernel directory.
void mmuUnmapAnon(AddressSpace* s, uint32_t base, uint32_t bytes) {
	uint32_t end = (base + bytes + 0xFFFu) & ~0xFFFu;
	uint32_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	for (uint32_t va = base; va < end; va += 0x1000) {
		uint32_t pa = s->impl.translate(va);
		s->impl.unmap(va);
		if (pa != 0xFFFFFFFFu)
			g_fa->free(pa);
	}
	kernel::loadCr3(saved);   // CR3 reload flushes the TLB so the cleared PTEs are live
}

}  // namespace arch
