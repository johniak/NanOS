/*
 * bootinfo_x86.cpp — x86 implementation of <arch/bootinfo.h> over Multiboot.
 *
 * Owns the `mbd` global (the Multiboot info pointer stashed by loader.s) and
 * the QEMU-default fallback. Translates the Multiboot memory map into the
 * arch-neutral "usable range" iteration the MI frame allocator consumes.
 */
#include <arch/bootinfo.h>
#include "MultibootInfo.h"
#include "MultibootMmap.h"

// Set by arch/x86/boot/loader.s from ebx at boot.
extern "C" unsigned mbd;

namespace {

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

namespace arch {

uint32_t bootMemTop() {
	kernel::MultibootInfo* mbi = (kernel::MultibootInfo*) mbd;
	uint32_t top = kernel::highestUsableAddr(mbi);
	return top ? top : 0x8000000;   // fallback: 128 MiB (QEMU default)
}

void bootMemForEachUsable(void* ctx, UsableRangeCb cb) {
	FwdCtx f = { ctx, cb };
	kernel::MultibootInfo* mbi = (kernel::MultibootInfo*) mbd;
	kernel::parseMmap(mbi, &f, forwardUsable);
}

}  // namespace arch
