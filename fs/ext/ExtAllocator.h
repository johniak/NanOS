/*
 * ExtAllocator.h — block and inode allocation for the ext2/3/4 family. Allocates/frees bits in
 * the per-group block and inode bitmaps and keeps every dependent count and checksum in sync:
 * the group descriptor's free-blocks/free-inodes/used-dirs counts (lo+hi for 64-bit), the
 * superblock's free counts, the bitmap checksums, the group-descriptor checksum and the
 * superblock checksum (when metadata_csum is on).
 *
 * Self-contained and shared by ext2 and ext4: it derives the whole geometry from the
 * superblock bytes and reaches the disk only through BlockCache, so it is host-testable on a
 * RamBlockDevice image and used unchanged by both drivers. Block/inode MAPPING (which is the
 * only format-specific part) stays in the Ext2/Ext4 subclasses; this only owns free-space.
 */
#ifndef EXT_EXTALLOCATOR_H_
#define EXT_EXTALLOCATOR_H_

#include "ext/BlockCache.h"

namespace kernel {

class ExtAllocator {
public:
	// `sb` points at the live 1024-byte superblock buffer (read+written); `cache` is the fs
	// block cache. All geometry is read from `sb`. The superblock is written back through the
	// cache when a count changes; the caller flushes the cache to persist.
	ExtAllocator(BlockCache* cache, unsigned char* sb);

	unsigned allocBlock(unsigned goalGroup);     // -> absolute fs block, or 0 if the disk is full
	void     freeBlock(unsigned blockNo);
	unsigned allocInode(bool isDir, unsigned goalGroup);  // -> 1-based inode number, or 0 if full
	void     freeInode(unsigned inodeNo, bool isDir);

	// Geometry accessors the rest of the ext core reuses (so it doesn't re-parse the superblock).
	unsigned blockSize() const { return m_blockSize; }
	unsigned groupCount() const { return m_groupCount; }
	unsigned inodesPerGroup() const { return m_inodesPerGroup; }
	unsigned inodeSize() const { return m_inodeSize; }
	unsigned firstInode() const { return m_firstIno; }
	unsigned csumSeed() const { return m_seed; }
	bool     metaCsum() const { return m_metaCsum; }

	// On-disk location of inode `ino` (1-based): the block holding it and the byte offset within.
	void inodeLocation(unsigned ino, unsigned& blockOut, unsigned& offsetOut) const;

	// Read/write the full inodeSize bytes of inode `ino` (1-based). writeInode recomputes
	// i_checksum (when metadata_csum is on) before writing, so the slot stays e2fsck-clean. An
	// inode never crosses a block boundary (inodeSize divides the block size), so one block I/O
	// covers it. `inode` must hold at least inodeSize() bytes.
	void readInode(unsigned ino, unsigned char* out);
	void writeInode(unsigned ino, unsigned char* inode);

private:
	BlockCache*    m_cache;
	unsigned char* m_sb;
	unsigned m_blockSize, m_descSize, m_groupCount;
	unsigned m_blocksPerGroup, m_inodesPerGroup;
	unsigned m_firstDataBlock, m_bgdtBlock, m_inodeSize, m_firstIno;
	bool     m_is64, m_metaCsum;
	unsigned m_seed;

	// Group descriptor I/O: read/write the descSize bytes of descriptor `g` from the bgdt.
	void descLoc(unsigned g, unsigned& blockOut, unsigned& offOut) const;
	void readDesc(unsigned g, unsigned char* buf);
	void writeDesc(unsigned g, unsigned char* buf);   // recomputes bg_checksum, writes via cache

	void writeSuperblock();                            // recompute s_checksum, write via cache

	// Generic bitmap alloc/free over one group: returns the freed/allocated bit index, or -1.
	int  bitmapAlloc(unsigned bitmapBlock, unsigned bits);
	void bitmapBit(unsigned bitmapBlock, unsigned bit, bool set);
	void refreshBitmapCsum(unsigned char* desc, unsigned bitmapBlock, bool isBlockBitmap);

	unsigned descU16(const unsigned char* d, unsigned off) const;
	unsigned descU32(const unsigned char* d, unsigned off) const;
	void     descSetU16(unsigned char* d, unsigned off, unsigned v);
	void     descSetU32(unsigned char* d, unsigned off, unsigned v);
	// 64-bit count split lo (32B part) + hi (64B part); read/modify by lo/hi offsets.
	unsigned descCount(const unsigned char* d, unsigned loOff, unsigned hiOff) const;
	void     descSetCount(unsigned char* d, unsigned loOff, unsigned hiOff, unsigned v);
	unsigned descBitmapBlock(const unsigned char* d, unsigned loOff, unsigned hiOff) const;

	unsigned sbU32(unsigned off) const;
	void     sbSetU32(unsigned off, unsigned v);
};

}  // namespace kernel

#endif /* EXT_EXTALLOCATOR_H_ */
