/*
 * FrameAllocator.h
 *
 * Physical 4 KiB-frame allocator backed by a fixed bitmap. The bitmap covers a
 * full 4 GiB of physical address space (128 KiB in .bss), so there is no
 * "where to place the bitmap" bootstrap problem — it lives inside the kernel
 * image and is self-reserved when the kernel range is marked used.
 *
 * No global constructor is relied upon: the kernel uses a zeroed .bss instance
 * (g_frames) and calls init() explicitly.
 */
#pragma once
#include <stdint.h>

namespace kernel {

const uint32_t FRAME_SIZE = 4096;

class FrameAllocator {
public:
	// Mark every frame in [0, topOfRam) used. Callers then open usable regions
	// with markRangeFree and re-reserve specific windows with markRangeUsed.
	void init(uint32_t topOfRam);

	// Free only fully-contained frames (base rounded up, end rounded down) so a
	// frame straddling into reserved memory is never handed out.
	void markRangeFree(uint32_t base, uint32_t len);
	// Reserve every touched frame (base rounded down, end rounded up).
	void markRangeUsed(uint32_t base, uint32_t len);

	uint32_t alloc();          // first-free frame's physical address; 0 = OOM
	void free(uint32_t pa);    // return a frame to the pool

	bool isUsed(uint32_t frameIndex) const;
	uint32_t freeCount() const;

private:
	static const uint32_t MAX_FRAMES = 1u << 20;          // 4 GiB / 4 KiB
	static const uint32_t BITMAP_WORDS = MAX_FRAMES / 32; // 32768 words = 128 KiB
	uint32_t m_bitmap[BITMAP_WORDS];
	uint32_t m_frameCount;

	void set(uint32_t i);
	void clear(uint32_t i);
	bool test(uint32_t i) const;
	uint32_t findFirstFree() const;   // returns m_frameCount if none
};

extern FrameAllocator g_frames;   // zeroed .bss instance (no global ctor)

}
