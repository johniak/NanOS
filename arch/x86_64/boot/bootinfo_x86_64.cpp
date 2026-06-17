/*
 * bootinfo_x86_64.cpp — x86_64 implementation of <arch/bootinfo.h> over Multiboot1.
 *
 * Mirrors arch/x86/boot/bootinfo_x86.cpp, but the Multiboot info pointer is handed over
 * explicitly by entry64 (the long-mode trampoline passes it in rdi as kentry64's arg0)
 * rather than stashed by the asm loader into a `mbd` global. The pointer and the mmap it
 * references are 32-bit physical addresses from GRUB, reachable through the loader's 1 GiB
 * identity map. Translates the Multiboot memory map into the arch-neutral usable-range
 * iteration the MI frame allocator consumes.
 */
#include <arch/bootinfo.h>
#include "MultibootInfo.h"
#include "MultibootMmap.h"

namespace {

// The Multiboot info pointer, set once by entry64 via bootSetMultibootInfo (below).
uintptr_t g_mbInfo = 0;

struct FwdCtx {
	void* ctx;
	arch::UsableRangeCb cb;
};

// parseMmap reports every entry; forward only the usable ones, dropping `type`.
void forwardUsable(void* c, uint64_t base, uint64_t length, uint32_t type) {
	if (type != kernel::MMAP_TYPE_AVAILABLE)
		return;
	FwdCtx* f = (FwdCtx*) c;
	f->cb(f->ctx, base, length);
}

}  // namespace

// Arch-internal (NOT an <arch/bootinfo.h> contract): entry64 calls this with kentry64's arg0.
extern "C" void bootSetMultibootInfo(unsigned long mb) {
	g_mbInfo = (uintptr_t) mb;
}

namespace arch {

uint32_t bootMemTop() {
	kernel::MultibootInfo* mbi = (kernel::MultibootInfo*) g_mbInfo;
	uint32_t top = mbi ? kernel::highestUsableAddr(mbi) : 0;
	return top ? top : 0x8000000;   // fallback: 128 MiB
}

void bootMemForEachUsable(void* ctx, UsableRangeCb cb) {
	FwdCtx f = { ctx, cb };
	kernel::MultibootInfo* mbi = (kernel::MultibootInfo*) g_mbInfo;
	if (mbi)
		kernel::parseMmap(mbi, &f, forwardUsable);
}

const BootFramebuffer* bootFramebuffer() {
	static BootFramebuffer fb;
	static int state = 0;   // 0=unprobed, 1=present, 2=absent
	if (state == 0) {
		kernel::MbFramebuffer mb;
		kernel::MultibootInfo* mbi = (kernel::MultibootInfo*) g_mbInfo;
		if (mbi && kernel::multibootFramebuffer(mbi, &mb)) {
			fb.addr = mb.addr;
			fb.pitch = mb.pitch;
			fb.width = mb.width;
			fb.height = mb.height;
			fb.bpp = mb.bpp;
			state = 1;
		} else {
			state = 2;
		}
	}
	return state == 1 ? &fb : 0;
}

}  // namespace arch
