#include "BlockCache.h"
#include "string.h"

namespace kernel {

BlockCache::BlockCache(BlockDevice* dev, unsigned partitionLba, unsigned blockSize) {
	m_dev = dev;
	m_partitionLba = partitionLba;
	m_blockSize = blockSize ? blockSize : 1024;
	m_clock = 0;
	for (int i = 0; i < SLOTS; i++) {
		m_slot[i].valid = false;
		m_slot[i].dirty = false;
		m_slot[i].blockNo = 0;
		m_slot[i].lru = 0;
	}
}

BlockCache::Slot* BlockCache::find(unsigned blockNo) {
	for (int i = 0; i < SLOTS; i++)
		if (m_slot[i].valid && m_slot[i].blockNo == blockNo)
			return &m_slot[i];
	return 0;
}

// Return a slot holding blockNo's data: a hit, a free slot loaded from disk, or the LRU victim
// (flushed if dirty) reloaded. Always returns a valid slot with the block's current bytes.
BlockCache::Slot* BlockCache::obtain(unsigned blockNo) {
	Slot* s = find(blockNo);
	if (s) {
		s->lru = ++m_clock;
		return s;
	}
	// Pick a victim: first invalid slot, else the least-recently-used.
	Slot* v = 0;
	for (int i = 0; i < SLOTS; i++)
		if (!m_slot[i].valid) { v = &m_slot[i]; break; }
	if (!v) {
		v = &m_slot[0];
		for (int i = 1; i < SLOTS; i++)
			if (m_slot[i].lru < v->lru)
				v = &m_slot[i];
		if (v->dirty)
			m_dev->writeSectors(lbaOf(v->blockNo), sectorsPerBlock(), v->data);
	}
	m_dev->readSectors(lbaOf(blockNo), sectorsPerBlock(), v->data);
	v->blockNo = blockNo;
	v->valid = true;
	v->dirty = false;
	v->lru = ++m_clock;
	return v;
}

void BlockCache::read(unsigned blockNo, void* out) {
	Slot* s = obtain(blockNo);
	memcpy(out, s->data, m_blockSize);
}

void BlockCache::write(unsigned blockNo, const void* in) {
	// A full-block overwrite: we don't need the old contents, but reuse obtain() for slot mgmt
	// (it may load then we overwrite — correct, just one extra read on a cold miss).
	Slot* s = obtain(blockNo);
	memcpy(s->data, in, m_blockSize);
	s->dirty = true;
}

void BlockCache::writePartial(unsigned blockNo, unsigned offset, const void* in, unsigned len) {
	if (offset >= m_blockSize)
		return;
	if (offset + len > m_blockSize)
		len = m_blockSize - offset;
	Slot* s = obtain(blockNo);
	memcpy(s->data + offset, in, len);
	s->dirty = true;
}

void BlockCache::flush() {
	for (int i = 0; i < SLOTS; i++)
		if (m_slot[i].valid && m_slot[i].dirty) {
			m_dev->writeSectors(lbaOf(m_slot[i].blockNo), sectorsPerBlock(), m_slot[i].data);
			m_slot[i].dirty = false;
		}
}

void BlockCache::invalidate() {
	flush();
	for (int i = 0; i < SLOTS; i++)
		m_slot[i].valid = false;
}

int BlockCache::dirtyList(unsigned* out, int max) {
	int n = 0;
	for (int i = 0; i < SLOTS && n < max; i++)
		if (m_slot[i].valid && m_slot[i].dirty)
			out[n++] = m_slot[i].blockNo;
	return n;
}

}  // namespace kernel
