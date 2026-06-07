/*
 * mmu_x86.cpp — x86 implementation of <arch/mmu.h>.
 *
 * Builds the kernel page directory identity-mapping all RAM (4 KiB pages),
 * reserves the windows the frame pool must never hand out, loads CR3 and turns
 * on paging. Wraps the host-tested AddressSpace + the CR-register helpers.
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

}  // namespace

namespace arch {

void mmuInitKernel(kernel::FrameAllocator& fa, uint32_t topOfRam) {
	g_fa = &fa;

	// Re-reserve the windows the frame pool must never hand out.
	fa.markRangeUsed(0, 0x100000);                                   // low mem + VGA
	fa.markRangeUsed(0x100000, (uint32_t) (unsigned) &end - 0x100000); // kernel image
	fa.markRangeUsed(0x400000, 0x100000);                           // user window
	uint32_t heapBase = 0x75BCD15 & kernel::PAGE_MASK;              // bump heap
	fa.markRangeUsed(heapBase, topOfRam - heapBase);

	// Directory + page tables come from the frame pool (below the heap); the
	// AddressSpace object itself comes from the already-reserved bump heap.
	static kernel::AddressSpace* kspace = 0;
	kernel::PagingEnv env = { allocFrame, freeFrame, physToVirt, 0 };
	kspace = new kernel::AddressSpace(env);
	kspace->mapRange(0, 0, topOfRam, kernel::PTE_PRESENT | kernel::PTE_RW);

	__asm__ __volatile__("cli");
	kernel::loadCr3(kspace->directoryPhys());
	kernel::enablePaging();
	__asm__ __volatile__("sti");
}

}  // namespace arch
