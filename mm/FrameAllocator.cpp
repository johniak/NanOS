#include "FrameAllocator.h"
#include <string.h>

namespace kernel {

FrameAllocator g_frames;   // .bss, zeroed by the loader

void FrameAllocator::init(uint64_t topOfRam) {
	m_frameCount = topOfRam / FRAME_SIZE;
	if (m_frameCount > MAX_FRAMES)         // clamp to what the bitmap can represent
		m_frameCount = MAX_FRAMES;
	memset(m_bitmap, 0xFF, sizeof(m_bitmap));   // everything used until freed
}

void FrameAllocator::set(uint64_t i) {
	m_bitmap[i >> 5] |= (1u << (i & 31));
}
void FrameAllocator::clear(uint64_t i) {
	m_bitmap[i >> 5] &= ~(1u << (i & 31));
}
bool FrameAllocator::test(uint64_t i) const {
	return (m_bitmap[i >> 5] >> (i & 31)) & 1u;
}

void FrameAllocator::markRangeFree(uint64_t base, uint64_t len) {
	SpinIrqGuard g(m_lock);
	uint64_t start = (base + FRAME_SIZE - 1) / FRAME_SIZE;   // round up
	uint64_t end = (base + len) / FRAME_SIZE;                // round down
	if (end > m_frameCount)
		end = m_frameCount;
	for (uint64_t f = start; f < end; f++)
		clear(f);
}

void FrameAllocator::markRangeUsed(uint64_t base, uint64_t len) {
	SpinIrqGuard g(m_lock);
	uint64_t start = base / FRAME_SIZE;                        // round down
	uint64_t end = (base + len + FRAME_SIZE - 1) / FRAME_SIZE; // round up
	if (end > m_frameCount)
		end = m_frameCount;
	for (uint64_t f = start; f < end; f++)
		set(f);
}

uint64_t FrameAllocator::findFirstFree() const {
	for (uint64_t w = 0; w < BITMAP_WORDS; w++) {
		if (m_bitmap[w] != 0xFFFFFFFFu) {
			for (uint64_t b = 0; b < 32; b++) {
				if (!((m_bitmap[w] >> b) & 1u)) {
					uint64_t f = w * 32 + b;
					return f < m_frameCount ? f : m_frameCount;
				}
			}
		}
	}
	return m_frameCount;
}

uint64_t FrameAllocator::alloc() {
	SpinIrqGuard g(m_lock);
	uint64_t f = findFirstFree();   // private; called only here, so no nested lock
	if (f >= m_frameCount)
		return 0;   // OOM (frame 0 is always reserved low memory, so 0 is unambiguous)
	set(f);
	return f * FRAME_SIZE;
}

void FrameAllocator::free(uint64_t pa) {
	SpinIrqGuard g(m_lock);
	clear(pa / FRAME_SIZE);
}

bool FrameAllocator::isUsed(uint64_t frameIndex) const {
	SpinIrqGuard g(m_lock);
	return test(frameIndex);
}

uint64_t FrameAllocator::freeCount() const {
	SpinIrqGuard g(m_lock);
	uint64_t n = 0;
	for (uint64_t f = 0; f < m_frameCount; f++)
		if (!test(f))
			n++;
	return n;
}

}
