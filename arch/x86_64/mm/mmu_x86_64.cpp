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
#include <arch/smp.h>         // smpTlbShootdown: drop stale TLB entries on other CPUs before reuse
#include "AddressSpace.h"
#include "Paging.h"
#include "PagingControl.h"
#include "FrameAllocator.h"
#include "memory_manager.h"   // heapInit
#include <string.h>

// Linker symbol marking the end of the kernel image (arch/x86_64/linker.ld).
extern char end;

// Local APIC bring-up (arch/x86_64/cpu/lapic_x86_64.cpp). Called from mmuInitKernel's tail, once the
// LAPIC MMIO page is mapped — NOT from cpuInit, which runs before paging is set up.
namespace kernel { void lapicInit(); }

namespace {

const uint64_t PD_SPAN = 0x200000;   // one PD entry maps 2 MiB on x86_64 (privatization stride)

kernel::FrameAllocator* g_fa = 0;
uint64_t allocFrame(void*) { return (uint64_t) g_fa->alloc(); }
void freeFrame(void*, uint64_t pa) { g_fa->free((uint32_t) pa); }
void* physToVirt(void*, uint64_t pa) { return (void*) (uintptr_t) pa; }  // identity-mapped

kernel::PagingEnv g_env = { allocFrame, freeFrame, physToVirt, 0 };
kernel::AddressSpace* g_kspace = 0;
uint64_t g_kernelDirPhys = 0;
// Top of the kernel huge-page identity map ([0, g_identityTop) is mapped RW with 2 MiB huge
// pages). MMIO below this is ALREADY identity-mapped; re-mapping it with a 4 KiB mapRange would
// collide with the live 2 MiB huge PDE (mapRange would walk the huge entry as a page-table ptr
// -> corruption). This bit the LAPIC (0xFEE00000) and framebuffer on real 8-16 GiB hardware,
// where topOfRam exceeds those MMIO addresses; QEMU (512 MiB) put them above the map, hiding it.
uint64_t g_identityTop = 0;

}  // namespace

namespace arch {

// Opaque per-process address space = one kernel::AddressSpace.
struct AddressSpace {
	kernel::AddressSpace impl;
	explicit AddressSpace(const kernel::PagingEnv& e) : impl(e) {}
};

void mmuInitKernel(kernel::FrameAllocator& fa, uint64_t topOfRam) {
	g_fa = &fa;

	// Re-reserve the windows the frame pool must never hand out.
	fa.markRangeUsed(0, 0x100000);                                   // low mem + VGA
	fa.markRangeUsed(0x100000, (uint64_t) (uintptr_t) &end - 0x100000); // kernel image
	fa.markRangeUsed(VA_USER_BASE, 0x2000000);                     // exec staging window (32 MiB — must
	                                                               // cover the largest staged .nxe; see
	                                                               // kernel/Exec.cpp STAGE_CAP)
	// Kernel byte heap: carve ~25% off RAM, clamped to [8 MiB, 256 MiB]. It is laid out + first
	// touched (heapInit, then `new AddressSpace` below) while ONLY the loader's temporary 1 GiB
	// identity map is live, so the heap MUST sit within that first 1 GiB. Place it at the top of
	// min(topOfRam, 1 GiB); the full huge-page map built below then still covers it, and all RAM
	// above the heap (incl. > 4 GiB) goes to the frame pool. (On <=1 GiB machines this is the old
	// heap-at-top-of-RAM behaviour unchanged.)
	const uint64_t BOOT_IDENTITY = 0x40000000;            // loader.s maps 1 GiB (512 x 2 MiB)
	uint64_t heapSize = topOfRam / 4u;
	// Cap at 512 MiB (was 256). The LinuxKPI mem_map — one 64-byte struct page per RAM frame — is a
	// single heap block up to ~256 MiB on a 16 GiB machine, and i915 GEM/framebuffer allocations come
	// from this same heap; 256 MiB total starved both and mem_map alloc FAILED on the Dell (real HW,
	// 8-16 GiB). 512 MiB fits mem_map + an early-probe working set. heapBase = 1 GiB - 512 MiB = 512
	// MiB sits well above the kernel image + 32 MiB exec-staging window and inside the loader's 1 GiB
	// identity map, so the constraints below still hold. Machines <=2 GiB never reach this cap.
	if (heapSize > 512ull * 1024 * 1024) heapSize = 512ull * 1024 * 1024;
	if (heapSize < 8ull * 1024 * 1024)   heapSize = 8ull * 1024 * 1024;
	uint64_t heapTop = topOfRam < BOOT_IDENTITY ? topOfRam : BOOT_IDENTITY;
	uint64_t heapBase = (heapTop - heapSize) & kernel::PAGE_MASK;
	fa.markRangeUsed(heapBase, heapSize);     // reserve exactly the heap region (not up to topOfRam)
	// Lay out the kernel heap before the first malloc (new AddressSpace below). heapBase is within
	// the loader's 1 GiB temporary identity map, so this region is directly accessible right now.
	heapInit((void*) (uintptr_t) heapBase, heapSize);

	g_kspace = new kernel::AddressSpace(g_env);
	// Identity-map all RAM with 2 MiB huge pages (round the top up to 2 MiB) — cheap for many GiB.
	uint64_t mapTop = (topOfRam + 0x1FFFFF) & ~0x1FFFFFull;
	g_kspace->mapRangeHuge(0, 0, mapTop, kernel::PTE_PRESENT | kernel::PTE_RW);
	g_identityTop = mapTop;   // MMIO below this is already mapped (see g_identityTop note)
	g_kernelDirPhys = g_kspace->directoryPhys();

	__asm__ __volatile__("cli");
	kernel::enableNxe();                 // honor the NX bit on later USER mappings
	kernel::loadCr3(g_kernelDirPhys);    // switch off the Plan-1 temporary map onto our PML4
	kernel::enablePaging();              // re-assert CR0.PG (already on in long mode)
	// Local APIC MMIO (architectural default 0xFEE00000). On small-RAM machines (QEMU) it sits
	// ABOVE the huge identity map and needs an explicit 4 KiB mapping. On real 8-16 GiB hardware it
	// falls WITHIN [0, mapTop) and is already identity-mapped by a 2 MiB huge page — mapping it
	// again with a 4 KiB mapRange would collide with that huge PDE (mapRange walks the huge entry as
	// a page-table pointer -> corruption -> the fault that stopped boot after the WHITE POST bar).
	// Map explicitly only when it is above the huge map.
	if (0xFEE00000ull + 0x1000 > mapTop)
		g_kspace->mapRange(0xFEE00000, 0xFEE00000, 0x1000, kernel::PTE_PRESENT | kernel::PTE_RW);
	__asm__ __volatile__("sti");
	// Now that the LAPIC register page is mapped on the live kernel space, bring the Local APIC up.
	// (cpuInit — which installs the IDT incl. the MSI gates — already ran; lapicInit fires no
	// interrupts, it only enables the APIC + spurious vector + the MSI vector pool.)
	kernel::lapicInit();
}

uint32_t mmuKernelDirPhys() { return (uint32_t) g_kernelDirPhys; }

void mmuMapKernelMmio(uint64_t phys, uint64_t bytes) {
	// Device MMIO must be UNCACHED (PCD+PWT). A cached register write buffers in the CPU cache and
	// never reaches the device — on real hardware an i915 forcewake write is then never acked and the
	// GPU bring-up hard-hangs on the first forcewake-gated register read. QEMU/TCG treats cached MMIO
	// as if it landed, which hid this for the whole QEMU bring-up.
	const uint64_t UC = kernel::PTE_PRESENT | kernel::PTE_RW | kernel::PTE_PCD | kernel::PTE_PWT;
	const uint64_t HP = 0x200000;   // 2 MiB huge-page span
	uint64_t base = phys & kernel::PAGE_MASK;
	uint64_t end_ = (phys + bytes + ~kernel::PAGE_MASK) & kernel::PAGE_MASK;  // round up to 4 KiB
	if (base >= end_)
		return;

	// (1) The part inside the 2 MiB huge identity map is ALREADY mapped, but write-back. We cannot
	// drop a 4 KiB UC mapping over a live 2 MiB huge PDE, so overwrite the covering huge PDEs with the
	// same identity phys + UC. These addresses are PCI MMIO holes (below top-of-RAM but not RAM), so
	// marking the whole covering 2 MiB span uncached is safe. Real-HW GPU BARs (Dell: 0x80000000 /
	// 0xCB000000) take this path; QEMU's high MMIO does not.
	if (base < g_identityTop) {
		uint64_t hiEnd = end_ < g_identityTop ? end_ : g_identityTop;
		uint64_t hbase = base & ~(HP - 1);
		uint64_t hend  = (hiEnd + HP - 1) & ~(HP - 1);
		g_kspace->mapRangeHuge(hbase, hbase, hend - hbase, UC);
	}
	// (2) The part above the huge identity map needs a fresh 4 KiB UC mapping (QEMU's MMIO layout).
	if (end_ > g_identityTop) {
		uint64_t lo = base < g_identityTop ? g_identityTop : base;
		g_kspace->mapRange(lo, lo, end_ - lo, UC);
	}
	// Flush the local TLB so the new (uncached) attributes take effect immediately.
	kernel::loadCr3(kernel::readCr3());
}

uint32_t mmuCurrentDirPhys() { return (uint32_t) kernel::readCr3(); }
void mmuLoadDirPhys(uint32_t dirPhys) { kernel::loadCr3(dirPhys); }

// Privatize (per-process) EVERY user window beyond the program-image window that adoptKernelDirectory
// already dropped: module/heap/mmap/fb all live in pdpt[1], where the >1 GiB kernel identity map would
// otherwise leak its huge entries into the shared half. dropPde forks pdpt[1]'s PD once (idempotent)
// and clears each window slot in the private copy.
static void dropUserWindows(AddressSpace* s) {
	for (uint64_t va = VA_USER_BASE + PD_SPAN; va < VA_USER_END;   va += PD_SPAN) s->impl.dropPde(va);
	for (uint64_t va = VA_MODULE_BASE;          va < VA_MODULE_MAX; va += PD_SPAN) s->impl.dropPde(va);
	for (uint64_t va = VA_HEAP_BASE;            va < VA_HEAP_MAX;   va += PD_SPAN) s->impl.dropPde(va);
	for (uint64_t va = VA_MMAP_BASE;            va < VA_MMAP_MAX;   va += PD_SPAN) s->impl.dropPde(va);
	for (uint64_t va = VA_FB_BASE;              va < VA_FB_MAX;     va += PD_SPAN) s->impl.dropPde(va);
}

// Free every user window's leaf PT + pages (the private PDPTs/PDs are freed afterwards by freeUserTables).
static void freeUserWindowsAll(AddressSpace* s) {
	for (uint64_t va = VA_USER_BASE;   va < VA_USER_END;   va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_MODULE_BASE; va < VA_MODULE_MAX; va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_HEAP_BASE;   va < VA_HEAP_MAX;   va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_MMAP_BASE;   va < VA_MMAP_MAX;   va += PD_SPAN) s->impl.freeUserWindow(va);
	for (uint64_t va = VA_FB_BASE;     va < VA_FB_MAX;     va += PD_SPAN) s->impl.freeUserWindow(va);
}

AddressSpace* mmuCreateAddressSpace() {
	AddressSpace* s = new AddressSpace(g_env);
	// Share the whole kernel half; privatize each PD entry of every user window. adopt privatizes
	// the path to VA_USER_BASE and drops that PD entry; dropUserWindows clears the rest (incl. pdpt[1]).
	s->impl.adoptKernelDirectory(g_kernelDirPhys, VA_USER_BASE);
	dropUserWindows(s);
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
	freeUserWindowsAll(s);
	s->impl.freeUserTables();   // free the private PDPTs/PDs freeUserWindow leaves behind
	g_fa->free((uint32_t) s->impl.directoryPhys());
	delete s;
}

AddressSpace* mmuCopyAddressSpace(AddressSpace* src) {
	AddressSpace* s = new AddressSpace(g_env);
	s->impl.adoptKernelDirectory(g_kernelDirPhys, VA_USER_BASE);
	dropUserWindows(s);
	bool ok = true;
	for (uint64_t va = VA_USER_BASE;   ok && va < VA_USER_END;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_HEAP_BASE;   ok && va < VA_HEAP_MAX;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_MODULE_BASE; ok && va < VA_MODULE_MAX; va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_MMAP_BASE;   ok && va < VA_MMAP_MAX;   va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	for (uint64_t va = VA_FB_BASE;     ok && va < VA_FB_MAX;     va += PD_SPAN) ok = s->impl.copyUserWindowFrom(src->impl, va);
	if (!ok) {
		mmuFreeAddressSpace(s);
		return 0;
	}
	return s;
}

uint32_t mmuSpaceDirPhys(AddressSpace* s) { return (uint32_t) s->impl.directoryPhys(); }

uint32_t mmuFbBase() { return (uint32_t) VA_FB_BASE; }
uint32_t mmuFbMax()  { return (uint32_t) VA_FB_MAX; }

int mmuMapUserFbAt(AddressSpace* s, uint32_t va, uint64_t fbPhys, uint32_t bytes) {
	uint64_t base = fbPhys & kernel::PAGE_MASK;
	uint64_t off = fbPhys - base;
	uint64_t len = (off + bytes + ~kernel::PAGE_MASK) & kernel::PAGE_MASK;
	// Allocating + zeroing page-table frames touches arbitrary RAM by identity, only safe under
	// the kernel directory (the process PML4's user windows do NOT identity-map all RAM).
	uint64_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	bool ok = s->impl.mapRange(va, base, len,
			kernel::PTE_PRESENT | kernel::PTE_RW | kernel::PTE_USER);
	kernel::loadCr3(saved);
	return ok ? 0 : -1;
}

void mmuUnmapUserFb(AddressSpace* s, uint32_t va, uint32_t bytes) {
	uint64_t end = (uint64_t) va + ((bytes + ~kernel::PAGE_MASK) & kernel::PAGE_MASK);
	uint64_t saved = kernel::readCr3();
	kernel::loadCr3(g_kernelDirPhys);
	for (uint64_t a = va; a < end; a += 0x1000)
		s->impl.unmap(a);
	// The mapped frames belong to the device / GEM object — clear the PTEs and flush every
	// CPU's stale translation, but never hand the frames to the frame allocator here.
	arch::smpTlbShootdown(s->impl.directoryPhys());
	kernel::loadCr3(saved);
}

uint32_t mmuUserHeapBase() { return (uint32_t) VA_HEAP_BASE; }
uint32_t mmuUserHeapMax()  { return (uint32_t) VA_HEAP_MAX; }
uint32_t mmuModuleBase()   { return (uint32_t) VA_MODULE_BASE; }
uint32_t mmuModuleMax()    { return (uint32_t) VA_MODULE_MAX; }
uint32_t mmuModuleStride() { return (uint32_t) VA_MODULE_STRIDE; }

// Unmap [from,to) from address space `s`, drop the stale TLB entry on EVERY CPU, then release the
// frames — strictly in that order. A sibling thread of `s` running on another core could otherwise
// still reach a freed (and possibly reused) frame through a cached translation. Batched so the PA
// buffer stays small. The caller must already be on g_kernelDirPhys (page-table edits touch RAM by
// identity). On a uniprocessor smpTlbShootdown is just a local CR3 reload.
static void unmapRangeAndFree(AddressSpace* s, uint64_t from, uint64_t to) {
	const int BATCH = 64;
	uint32_t pas[BATCH];
	for (uint64_t va = from; va < to; ) {
		int k = 0;
		for (; k < BATCH && va < to; va += 0x1000) {
			uint64_t pa = s->impl.translate(va);
			s->impl.unmap(va);
			if (pa != 0xFFFFFFFFFFFFFFFFULL) pas[k++] = (uint32_t) pa;
		}
		arch::smpTlbShootdown(s->impl.directoryPhys());   // PTEs cleared -> flush every CPU's TLB...
		for (int i = 0; i < k; i++) g_fa->free(pas[i]);   // ...THEN it is safe to reuse the frames
	}
}

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
		unmapRangeAndFree(s, newTop, oldTop);   // shrink: unmap -> shootdown -> free (see helper)
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
	unmapRangeAndFree(s, base, end_);   // unmap -> cross-CPU TLB shootdown -> free (see helper)
	kernel::loadCr3(saved);
}

}  // namespace arch
