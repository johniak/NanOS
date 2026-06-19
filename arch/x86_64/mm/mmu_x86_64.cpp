/*
 * mmu_x86_64.cpp — x86_64 implementation of <arch/mmu.h>.
 *
 * Kernel bring-up (mmuInitKernel: identity-map all RAM into a fresh PML4, enable NX, switch
 * CR3 off the Plan-1 temporary 1 GiB map) plus the per-process address-space API used by the
 * ring-3 loader. A process PML4 shares the whole kernel half (so syscalls/IRQs run with the
 * kernel mapped under any CR3) and owns a private page table for each user window. Mirrors
 * arch/x86/mm/mmu_x86.cpp; the i386 PDE granularity (4 MiB) becomes the x86_64 PD entry (2 MiB).
 *
 * FrameAllocator stays 32-bit-physical here (Plan 7 redesigns it for >4 GiB); the wrappers
 * widen its uint32_t addresses to the uint64_t the AddressSpace expects — lossless for RAM
 * <= 4 GiB. mmu.h signatures stay uint32_t (additive Plan 3); addresses are < 4 GiB so the
 * conversion to the AddressSpace's uint64_t never loses bits.
 */
#include <arch/mmu.h>
#include "AddressSpace.h"
#include "Paging.h"
#include "PagingControl.h"
#include "FrameAllocator.h"
#include "memory_manager.h"   // heapInit
#include <string.h>

// Linker symbol marking the end of the kernel image (arch/x86_64/linker.ld).
extern char end;

namespace {

const uint64_t PD_SPAN = 0x200000;   // one PD entry maps 2 MiB on x86_64 (privatization stride)

kernel::FrameAllocator* g_fa = 0;
uint64_t allocFrame(void*) { return (uint64_t) g_fa->alloc(); }
void freeFrame(void*, uint64_t pa) { g_fa->free((uint32_t) pa); }
void* physToVirt(void*, uint64_t pa) { return (void*) (uintptr_t) pa; }  // identity-mapped

kernel::PagingEnv g_env = { allocFrame, freeFrame, physToVirt, 0 };
kernel::AddressSpace* g_kspace = 0;
uint64_t g_kernelDirPhys = 0;

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
	fa.markRangeUsed(0x100000, (uint32_t) (uintptr_t) &end - 0x100000); // kernel image
	fa.markRangeUsed((uint32_t) VA_USER_BASE, 0x2000000);          // exec staging window (32 MiB — must
	                                                               // cover the largest staged .nxe; see
	                                                               // kernel/Exec.cpp STAGE_CAP)
	// Kernel byte heap: carve ~25% off the TOP of RAM, clamped to [8 MiB, 256 MiB]; the rest
	// (below) is the frame pool for page tables + user pages.
	uint32_t heapSize = topOfRam / 4u;
	if (heapSize > 256u * 1024u * 1024u) heapSize = 256u * 1024u * 1024u;
	if (heapSize < 8u * 1024u * 1024u)   heapSize = 8u * 1024u * 1024u;
	uint32_t heapBase = (uint32_t) ((topOfRam - heapSize) & (uint32_t) kernel::PAGE_MASK);
	fa.markRangeUsed(heapBase, topOfRam - heapBase);
	// Lay out the kernel heap before the first malloc (new AddressSpace below). The Plan-1
	// temporary identity map already covers this region, so it is directly accessible.
	heapInit((void*) (uintptr_t) heapBase, topOfRam - heapBase);

	g_kspace = new kernel::AddressSpace(g_env);
	g_kspace->mapRange(0, 0, topOfRam, kernel::PTE_PRESENT | kernel::PTE_RW);  // identity all RAM
	g_kernelDirPhys = g_kspace->directoryPhys();

	__asm__ __volatile__("cli");
	kernel::enableNxe();                 // honor the NX bit on later USER mappings
	kernel::loadCr3(g_kernelDirPhys);    // switch off the Plan-1 temporary map onto our PML4
	kernel::enablePaging();              // re-assert CR0.PG (already on in long mode)
	__asm__ __volatile__("sti");
}

uint32_t mmuKernelDirPhys() { return (uint32_t) g_kernelDirPhys; }

void mmuMapKernelMmio(uint32_t phys, uint32_t bytes) {
	uint64_t base = phys & kernel::PAGE_MASK;
	uint64_t end_ = ((uint64_t) phys + bytes + ~kernel::PAGE_MASK) & kernel::PAGE_MASK;  // round up
	g_kspace->mapRange(base, base, end_ - base, kernel::PTE_PRESENT | kernel::PTE_RW);
}

uint32_t mmuCurrentDirPhys() { return (uint32_t) kernel::readCr3(); }
void mmuLoadDirPhys(uint32_t dirPhys) { kernel::loadCr3(dirPhys); }

AddressSpace* mmuCreateAddressSpace() {
	AddressSpace* s = new AddressSpace(g_env);
	// Share the whole kernel half; privatize each PD entry of the user window. adopt privatizes
	// the path to VA_USER_BASE and drops that PD entry; dropPde clears the rest of the window.
	s->impl.adoptKernelDirectory(g_kernelDirPhys, VA_USER_BASE);
	for (uint64_t va = VA_USER_BASE + PD_SPAN; va < VA_USER_END; va += PD_SPAN)
		s->impl.dropPde(va);
	return s;
}

void mmuMap(AddressSpace* s, uint32_t va, uint32_t pa, uint32_t flags) { s->impl.map(va, pa, flags); }
void mmuUnmap(AddressSpace* s, uint32_t va) { s->impl.unmap(va); }
void mmuSwitch(AddressSpace* s) { kernel::loadCr3(s->impl.directoryPhys()); }
void mmuDestroyAddressSpace(AddressSpace* s) { mmuFreeAddressSpace(s); }

void mmuFreeAddressSpace(AddressSpace* s) {
	if (!s)
		return;
	// Free the PRIVATE parts: the per-window page tables + their leaf frames (freeUserWindow), then
	// the private intermediate tables (freeUserTables: the PDPTs/PDs marked PTE_PRIV), then the PML4.
	// The shared kernel-half tables are aliased by the copied PML4 entries — leave them.
	for (uint64_t va = VA_USER_BASE;   va < VA_USER_END;   va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_HEAP_BASE;   va < VA_HEAP_MAX;   va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_MODULE_BASE; va < VA_MODULE_MAX; va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_MMAP_BASE;   va < VA_MMAP_MAX;   va += PD_SPAN) s->impl.freeUserWindow(va);
	s->impl.freeUserTables();   // free the private PDPTs/PDs freeUserWindow leaves behind
	g_fa->free((uint32_t) s->impl.directoryPhys());
	delete s;
}

AddressSpace* mmuCopyAddressSpace(AddressSpace* src) {
	AddressSpace* s = new AddressSpace(g_env);
	s->impl.adoptKernelDirectory(g_kernelDirPhys, VA_USER_BASE);
	for (uint64_t va = VA_USER_BASE + PD_SPAN; va < VA_USER_END; va += PD_SPAN)
		s->impl.dropPde(va);
	bool ok = true;
	for (uint64_t va = VA_USER_BASE;   ok && va < VA_USER_END;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_HEAP_BASE;   ok && va < VA_HEAP_MAX;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_MODULE_BASE; ok && va < VA_MODULE_MAX; va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_MMAP_BASE;   ok && va < VA_MMAP_MAX;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	if (!ok) {
		mmuFreeAddressSpace(s);
		return 0;
	}
	return s;
}

uint32_t mmuSpaceDirPhys(AddressSpace* s) { return (uint32_t) s->impl.directoryPhys(); }

uint32_t mmuMapUserFb(AddressSpace* s, uint32_t fbPhys, uint32_t bytes) {
	uint64_t base = fbPhys & kernel::PAGE_MASK;
	uint64_t off = (uint64_t) fbPhys - base;
	uint64_t len = (off + bytes + ~kernel::PAGE_MASK) & kernel::PAGE_MASK;
	// Allocating + zeroing page-table frames touches arbitrary RAM by identity, only safe under
	// the kernel directory (the process PML4's user windows do NOT identity-map all RAM).
	uint64_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	bool ok = s->impl.mapRange(VA_FB_BASE, base, len,
			kernel::PTE_PRESENT | kernel::PTE_RW | kernel::PTE_USER);
	kernel::loadCr3(saved);
	return ok ? (uint32_t) (VA_FB_BASE + off) : 0;
}

uint32_t mmuUserHeapBase() { return (uint32_t) VA_HEAP_BASE; }
uint32_t mmuUserHeapMax()  { return (uint32_t) VA_HEAP_MAX; }
uint32_t mmuModuleBase()   { return (uint32_t) VA_MODULE_BASE; }
uint32_t mmuModuleMax()    { return (uint32_t) VA_MODULE_MAX; }
uint32_t mmuModuleStride() { return (uint32_t) VA_MODULE_STRIDE; }

int mmuSetUserBrk(AddressSpace* s, uint32_t oldBrk, uint32_t newBrk) {
	uint64_t oldTop = (oldBrk + 0xFFFu) & ~0xFFFull;
	uint64_t newTop = (newBrk + 0xFFFu) & ~0xFFFull;
	uint64_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	int rc = 0;
	if (newTop > oldTop) {
		for (uint64_t va = oldTop; va < newTop; va += 0x1000) {
			uint32_t f = g_fa->alloc();
			if (!f) { rc = -1; break; }
			memset((void*) (uintptr_t) f, 0, 0x1000);
			if (!s->impl.map(va, f, kernel::PTE_PRESENT | kernel::PTE_RW | kernel::PTE_USER)) {
				g_fa->free(f); rc = -1; break;
			}
		}
	} else if (newTop < oldTop) {
		for (uint64_t va = newTop; va < oldTop; va += 0x1000) {
			uint64_t pa = s->impl.translate(va);
			s->impl.unmap(va);
			if (pa != 0xFFFFFFFFFFFFFFFFULL)
				g_fa->free((uint32_t) pa);
		}
	}
	kernel::loadCr3(saved);   // CR3 reload flushes the TLB so the new PTEs are live
	return rc;
}

uint32_t mmuMmapBase() { return (uint32_t) VA_MMAP_BASE; }
uint32_t mmuMmapMax()  { return (uint32_t) VA_MMAP_MAX; }

int mmuMapAnon(AddressSpace* s, uint32_t base, uint32_t bytes, int writable) {
	uint64_t end_ = ((uint64_t) base + bytes + 0xFFFu) & ~0xFFFull;
	uint64_t flags = kernel::PTE_PRESENT | kernel::PTE_USER | (writable ? kernel::PTE_RW : 0);
	uint64_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	int rc = 0;
	for (uint64_t va = base; va < end_; va += 0x1000) {
		uint32_t f = g_fa->alloc();
		if (!f) { rc = -1; break; }
		memset((void*) (uintptr_t) f, 0, 0x1000);
		if (!s->impl.map(va, f, flags)) { g_fa->free(f); rc = -1; break; }
	}
	kernel::loadCr3(saved);
	return rc;
}

void mmuUnmapAnon(AddressSpace* s, uint32_t base, uint32_t bytes) {
	uint64_t end_ = ((uint64_t) base + bytes + 0xFFFu) & ~0xFFFull;
	uint64_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	for (uint64_t va = base; va < end_; va += 0x1000) {
		uint64_t pa = s->impl.translate(va);
		s->impl.unmap(va);
		if (pa != 0xFFFFFFFFFFFFFFFFULL)
			g_fa->free((uint32_t) pa);
	}
	kernel::loadCr3(saved);
}

}  // namespace arch
