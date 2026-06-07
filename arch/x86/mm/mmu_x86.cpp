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

// Linker symbol marking the end of the kernel image (arch/x86/linker.ld).
extern char end;

namespace {

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
	fa.markRangeUsed(0x400000, 0x100000);                           // user window
	uint32_t heapBase = 0x75BCD15 & kernel::PAGE_MASK;              // bump heap
	fa.markRangeUsed(heapBase, topOfRam - heapBase);

	g_kspace = new kernel::AddressSpace(g_env);
	g_kspace->mapRange(0, 0, topOfRam, kernel::PTE_PRESENT | kernel::PTE_RW);
	g_kernelDirPhys = g_kspace->directoryPhys();

	__asm__ __volatile__("cli");
	kernel::loadCr3(g_kernelDirPhys);
	kernel::enablePaging();
	__asm__ __volatile__("sti");
}

uint32_t mmuKernelDirPhys() { return g_kernelDirPhys; }

uint32_t mmuCurrentDirPhys() { return kernel::readCr3(); }
void mmuLoadDirPhys(uint32_t dirPhys) { kernel::loadCr3(dirPhys); }

AddressSpace* mmuCreateAddressSpace() {
	AddressSpace* s = new AddressSpace(g_env);
	// Share the whole kernel half; the user window (0x400000) gets a private PT.
	s->impl.adoptKernelDirectory(g_kernelDirPhys, 0x400000);
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
	// Free only the PRIVATE parts: the user-window page table + its frames, and the
	// directory. The kernel-half PDEs alias shared kernel page tables — leave them.
	s->impl.freeUserWindow(0x400000);
	g_fa->free(s->impl.directoryPhys());
	delete s;
}

AddressSpace* mmuCopyAddressSpace(AddressSpace* src) {
	AddressSpace* s = new AddressSpace(g_env);
	// Share the kernel half, private (empty) user window, then copy the user pages.
	s->impl.adoptKernelDirectory(g_kernelDirPhys, 0x400000);
	s->impl.copyUserWindowFrom(src->impl, 0x400000);
	return s;
}

uint32_t mmuSpaceDirPhys(AddressSpace* s) { return s->impl.directoryPhys(); }

}  // namespace arch
