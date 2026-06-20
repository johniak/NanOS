/*
 * FrameAllocator.h
 *
 * Physical 4 KiB-frame allocator backed by a fixed bitmap. The bitmap covers
 * 16 GiB of physical address space (512 KiB in .bss), so there is no
 * "where to place the bitmap" bootstrap problem — it lives inside the kernel
 * image and is self-reserved when the kernel range is marked used.
 *
 * All address arithmetic is 64-bit (LP64): a physical frame above 4 GiB keeps
 * its full address through alloc/free instead of losing the high bits.
 *
 * No global constructor is relied upon: the kernel uses a zeroed .bss instance
 * (g_frames) and calls init() explicitly.
 */
#pragma once
#include <stdint.h>

namespace kernel {

const uint64_t FRAME_SIZE = 4096;

class FrameAllocator {
public:
	// Physical span the bitmap can track (so the boot layer clamps top-of-RAM to what we can
	// actually pool). Equals the private MAX_PHYS — one source of truth.
	static constexpr uint64_t CAPACITY_BYTES = 16ull * 1024 * 1024 * 1024;   // 16 GiB

	// Mark every frame in [0, topOfRam) used. Callers then open usable regions
	// with markRangeFree and re-reserve specific windows with markRangeUsed.
	void init(uint64_t topOfRam);

	// Free only fully-contained frames (base rounded up, end rounded down) so a
	// frame straddling into reserved memory is never handed out.
	void markRangeFree(uint64_t base, uint64_t len);
	// Reserve every touched frame (base rounded down, end rounded up).
	void markRangeUsed(uint64_t base, uint64_t len);

	uint64_t alloc();          // first-free frame's physical address; 0 = OOM
	void free(uint64_t pa);    // return a frame to the pool

	bool isUsed(uint64_t frameIndex) const;
	uint64_t freeCount() const;

private:
	// Conscious cap: the static bitmap covers 16 GiB of physical space (512 KiB in .bss).
	// Enough for any QEMU/target RAM we boot; raising it is a one-line change here. Multiboot1
	// itself can't report regions past 4 GiB (spec §2.1) — this is the allocator-side headroom.
	static const uint64_t MAX_PHYS   = CAPACITY_BYTES;             // 16 GiB (see CAPACITY_BYTES)
	static const uint64_t MAX_FRAMES = MAX_PHYS / FRAME_SIZE;       // 1<<22 frames
	static const uint64_t BITMAP_WORDS = MAX_FRAMES / 32;          // 131072 words = 512 KiB
	uint32_t m_bitmap[BITMAP_WORDS];                               // 32 frames per word
	uint64_t m_frameCount;

	void set(uint64_t i);
	void clear(uint64_t i);
	bool test(uint64_t i) const;
	uint64_t findFirstFree() const;   // returns m_frameCount if none
};

extern FrameAllocator g_frames;   // zeroed .bss instance (no global ctor)

}
