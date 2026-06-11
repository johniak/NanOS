/*
 * BlockCache.h — block-granular I/O over a BlockDevice, the single choke point through which
 * the ext driver reads and writes filesystem blocks. It hides the block->sector math and the
 * partition LBA offset, and keeps a small write-back cache of recently touched blocks.
 *
 * Why a cache (not just readSectors): (1) ext metadata is touched repeatedly in one operation
 * (superblock, group descriptor, a bitmap, an inode table block) — caching avoids re-reading;
 * (2) writes are buffered + marked dirty and only hit the device on flush(), which is exactly
 * where the JBD2 journal will later interpose (journal the dirty set, then check it point).
 *
 * Pure logic over the BlockDevice HAL — host-testable on RamBlockDevice.
 */
#ifndef EXT_BLOCKCACHE_H_
#define EXT_BLOCKCACHE_H_

#include "BlockDevice.h"

namespace kernel {

class BlockCache {
public:
	static const unsigned MAX_BLOCK = 4096;   // largest ext block size we support
	static const int      SLOTS     = 32;     // cached blocks (LRU-evicted, dirty flushed first)

	// `partitionLba` is the device LBA where the filesystem (block 0) begins; `blockSize` is the
	// ext block size in bytes (1024..4096). Both fixed for the life of a mount.
	BlockCache(BlockDevice* dev, unsigned partitionLba, unsigned blockSize);

	void setBlockSize(unsigned blockSize) { m_blockSize = blockSize; }   // after superblock read

	// Read filesystem block `blockNo` into `out` (>= blockSize bytes); from cache if present.
	void read(unsigned blockNo, void* out);
	// Stage a full-block write: copy `in` into the cache and mark it dirty (write-back).
	void write(unsigned blockNo, const void* in);
	// Read-modify-write a sub-range of a block (offset+len within one block) and mark dirty.
	void writePartial(unsigned blockNo, unsigned offset, const void* in, unsigned len);

	void flush();          // write every dirty cached block to the device
	void invalidate();     // drop all cached blocks (discard clean; flush() dirty first if needed)

private:
	struct Slot {
		unsigned blockNo;
		bool     valid;
		bool     dirty;
		unsigned lru;                 // higher = more recently used
		unsigned char data[MAX_BLOCK];
	};
	BlockDevice* m_dev;
	unsigned     m_partitionLba;
	unsigned     m_blockSize;
	unsigned     m_clock;             // monotonically increasing LRU stamp
	Slot         m_slot[SLOTS];

	unsigned sectorsPerBlock() const { return m_blockSize / 512; }
	unsigned lbaOf(unsigned blockNo) const { return m_partitionLba + blockNo * sectorsPerBlock(); }
	Slot*    find(unsigned blockNo);
	Slot*    obtain(unsigned blockNo);   // find-or-load a slot for blockNo (may evict + flush)
};

}  // namespace kernel

#endif /* EXT_BLOCKCACHE_H_ */
