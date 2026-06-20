#include "MultibootMmap.h"
#include "FrameAllocator.h"   // CAPACITY_BYTES — cap top-of-RAM at what the frame pool can track

namespace kernel {

void parseMmapBuffer(const void* mmap, uint32_t len, void* ctx, MmapCallback cb) {
	uintptr_t addr = (uintptr_t) mmap;
	uintptr_t end = addr + len;
	// Walk by (size + 4): `size` excludes the size field itself.
	while (addr + sizeof(uint32_t) <= end) {
		const MmapEntry* e = (const MmapEntry*) addr;
		cb(ctx, e->base_addr, e->length, e->type);
		addr += e->size + sizeof(uint32_t);
	}
}

namespace {
struct TopAcc {
	uint64_t top;
};
void accumulateTop(void* ctx, uint64_t base, uint64_t length, uint32_t type) {
	if (type != MMAP_TYPE_AVAILABLE)
		return;
	TopAcc* a = (TopAcc*) ctx;
	uint64_t regionEnd = base + length;
	if (regionEnd > a->top)
		a->top = regionEnd;
}
// Cap top-of-RAM at the frame pool's bitmap capacity (16 GiB); 64-bit, never truncated to 4 GiB.
uint64_t capPool(uint64_t v) {
	return v > FrameAllocator::CAPACITY_BYTES ? FrameAllocator::CAPACITY_BYTES : v;
}
}

uint64_t highestUsableInBuffer(const void* mmap, uint32_t len) {
	TopAcc acc = { 0 };
	parseMmapBuffer(mmap, len, &acc, accumulateTop);
	return acc.top;
}

void parseMmap(const MultibootInfo* mbi, void* ctx, MmapCallback cb) {
	if (!(mbi->flags & MB_FLAG_MMAP))
		return;
	parseMmapBuffer((const void*) (uintptr_t) mbi->mmap_addr, mbi->mmap_length, ctx, cb);
}

uint64_t highestUsableAddr(const MultibootInfo* mbi) {
	if (mbi->flags & MB_FLAG_MMAP)
		return capPool(highestUsableInBuffer(
				(const void*) (uintptr_t) mbi->mmap_addr, mbi->mmap_length));
	if (mbi->flags & MB_FLAG_MEM)
		// mem_upper is KB above 1MB; total usable top = 1MB + mem_upper*1KB.
		return capPool(0x100000ull + (uint64_t) mbi->mem_upper * 1024ull);
	return 0;
}

}
