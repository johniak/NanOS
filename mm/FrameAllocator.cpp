#include "FrameAllocator.h"
#include <string.h>

namespace kernel {

FrameAllocator g_frames;   // .bss, zeroed by the loader

void FrameAllocator::init(uint32_t topOfRam) {
	m_frameCount = topOfRam / FRAME_SIZE;
	memset(m_bitmap, 0xFF, sizeof(m_bitmap));   // everything used until freed
}

void FrameAllocator::set(uint32_t i) {
	m_bitmap[i >> 5] |= (1u << (i & 31));
}
void FrameAllocator::clear(uint32_t i) {
	m_bitmap[i >> 5] &= ~(1u << (i & 31));
}
bool FrameAllocator::test(uint32_t i) const {
	return (m_bitmap[i >> 5] >> (i & 31)) & 1u;
}

void FrameAllocator::markRangeFree(uint32_t base, uint32_t len) {
	uint32_t start = (base + FRAME_SIZE - 1) / FRAME_SIZE;   // round up
	uint32_t end = (base + len) / FRAME_SIZE;                // round down
	if (end > m_frameCount)
		end = m_frameCount;
	for (uint32_t f = start; f < end; f++)
		clear(f);
}

void FrameAllocator::markRangeUsed(uint32_t base, uint32_t len) {
	uint32_t start = base / FRAME_SIZE;                      // round down
	uint32_t end = (base + len + FRAME_SIZE - 1) / FRAME_SIZE; // round up
	if (end > m_frameCount)
		end = m_frameCount;
	for (uint32_t f = start; f < end; f++)
		set(f);
}

uint32_t FrameAllocator::findFirstFree() const {
	for (uint32_t w = 0; w < BITMAP_WORDS; w++) {
		if (m_bitmap[w] != 0xFFFFFFFFu) {
			for (uint32_t b = 0; b < 32; b++) {
				if (!((m_bitmap[w] >> b) & 1u)) {
					uint32_t f = w * 32 + b;
					return f < m_frameCount ? f : m_frameCount;
				}
			}
		}
	}
	return m_frameCount;
}

uint32_t FrameAllocator::alloc() {
	uint32_t f = findFirstFree();
	if (f >= m_frameCount)
		return 0;   // OOM (frame 0 is always reserved low memory, so 0 is unambiguous)
	set(f);
	return f * FRAME_SIZE;
}

void FrameAllocator::free(uint32_t pa) {
	clear(pa / FRAME_SIZE);
}

bool FrameAllocator::isUsed(uint32_t frameIndex) const {
	return test(frameIndex);
}

uint32_t FrameAllocator::freeCount() const {
	uint32_t n = 0;
	for (uint32_t f = 0; f < m_frameCount; f++)
		if (!test(f))
			n++;
	return n;
}

}
