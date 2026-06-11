#include "ext/ExtAllocator.h"
#include "ext/ExtCsum.h"

namespace kernel {

// ---- superblock + descriptor field offsets (Linux ext4 layout) ----
enum {
	SB_INODES_COUNT = 0x00, SB_BLOCKS_LO = 0x04, SB_FREE_BLOCKS_LO = 0x0C, SB_FREE_INODES = 0x10,
	SB_FIRST_DATA_BLOCK = 0x14, SB_LOG_BLOCK_SIZE = 0x18, SB_BLOCKS_PER_GROUP = 0x20,
	SB_INODES_PER_GROUP = 0x28, SB_FIRST_INO = 0x54, SB_INODE_SIZE = 0x58, SB_BLOCKS_HI = 0x150,
	SB_FREE_BLOCKS_HI = 0x158, SB_DESC_SIZE = 0xFE, SB_INCOMPAT = 0x60,
	GD_BLOCK_BITMAP_LO = 0x00, GD_INODE_BITMAP_LO = 0x04, GD_FREE_BLOCKS_LO = 0x0C,
	GD_FREE_INODES_LO = 0x0E, GD_USED_DIRS_LO = 0x10, GD_FLAGS = 0x12, GD_BB_CSUM_LO = 0x18,
	GD_IB_CSUM_LO = 0x1A, GD_ITABLE_UNUSED_LO = 0x1C, GD_BLOCK_BITMAP_HI = 0x20,
	GD_INODE_BITMAP_HI = 0x24, GD_FREE_BLOCKS_HI = 0x2C, GD_FREE_INODES_HI = 0x2E,
	GD_USED_DIRS_HI = 0x30, GD_ITABLE_UNUSED_HI = 0x32, GD_BB_CSUM_HI = 0x38, GD_IB_CSUM_HI = 0x3A,
	BG_INODE_UNINIT = 0x1, BG_BLOCK_UNINIT = 0x2,
};

static unsigned rd32(const unsigned char* p, unsigned o) {
	return (unsigned) p[o] | ((unsigned) p[o + 1] << 8) | ((unsigned) p[o + 2] << 16) | ((unsigned) p[o + 3] << 24);
}
static void wr32(unsigned char* p, unsigned o, unsigned v) {
	p[o] = (unsigned char) v; p[o + 1] = (unsigned char) (v >> 8);
	p[o + 2] = (unsigned char) (v >> 16); p[o + 3] = (unsigned char) (v >> 24);
}
static unsigned rd16(const unsigned char* p, unsigned o) { return p[o] | (p[o + 1] << 8); }
static void wr16(unsigned char* p, unsigned o, unsigned v) {
	p[o] = (unsigned char) v; p[o + 1] = (unsigned char) (v >> 8);
}

unsigned ExtAllocator::sbU32(unsigned off) const { return rd32(m_sb, off); }
void ExtAllocator::sbSetU32(unsigned off, unsigned v) { wr32(m_sb, off, v); }
unsigned ExtAllocator::descU16(const unsigned char* d, unsigned off) const { return rd16(d, off); }
unsigned ExtAllocator::descU32(const unsigned char* d, unsigned off) const { return rd32(d, off); }
void ExtAllocator::descSetU16(unsigned char* d, unsigned off, unsigned v) { wr16(d, off, v); }
void ExtAllocator::descSetU32(unsigned char* d, unsigned off, unsigned v) { wr32(d, off, v); }

unsigned ExtAllocator::descCount(const unsigned char* d, unsigned loOff, unsigned hiOff) const {
	unsigned v = rd16(d, loOff);
	if (m_descSize >= 64) v |= (unsigned) rd16(d, hiOff) << 16;
	return v;
}
void ExtAllocator::descSetCount(unsigned char* d, unsigned loOff, unsigned hiOff, unsigned v) {
	wr16(d, loOff, v & 0xFFFF);
	if (m_descSize >= 64) wr16(d, hiOff, (v >> 16) & 0xFFFF);
}
unsigned ExtAllocator::descBitmapBlock(const unsigned char* d, unsigned loOff, unsigned hiOff) const {
	unsigned v = rd32(d, loOff);
	(void) hiOff;   // 32-bit block numbers (our images stay below 4 GiB); hi half ignored
	return v;
}

ExtAllocator::ExtAllocator(BlockCache* cache, unsigned char* sb) {
	m_cache = cache;
	m_sb = sb;
	m_blockSize = 1024u << sbU32(SB_LOG_BLOCK_SIZE);
	m_blocksPerGroup = sbU32(SB_BLOCKS_PER_GROUP);
	m_inodesPerGroup = sbU32(SB_INODES_PER_GROUP);
	m_firstDataBlock = sbU32(SB_FIRST_DATA_BLOCK);
	m_inodeSize = rd16(m_sb, SB_INODE_SIZE);
	if (m_inodeSize == 0) m_inodeSize = 128;
	m_firstIno = sbU32(SB_FIRST_INO);
	if (m_firstIno == 0) m_firstIno = 11;
	m_is64 = (sbU32(SB_INCOMPAT) & 0x80) != 0;
	m_descSize = m_is64 ? rd16(m_sb, SB_DESC_SIZE) : 32;
	if (m_descSize < 32) m_descSize = 32;
	unsigned blocks = sbU32(SB_BLOCKS_LO);
	m_groupCount = (blocks - m_firstDataBlock + m_blocksPerGroup - 1) / m_blocksPerGroup;
	m_bgdtBlock = (m_blockSize == 1024) ? 2 : 1;
	m_metaCsum = extHasMetadataCsum(m_sb);
	m_seed = extCsumSeed(m_sb);
	m_cache->setBlockSize(m_blockSize);
}

void ExtAllocator::descLoc(unsigned g, unsigned& blockOut, unsigned& offOut) const {
	unsigned byteOff = g * m_descSize;
	blockOut = m_bgdtBlock + byteOff / m_blockSize;
	offOut = byteOff % m_blockSize;
}

void ExtAllocator::readDesc(unsigned g, unsigned char* buf) {
	unsigned blk, off;
	descLoc(g, blk, off);
	unsigned char block[BlockCache::MAX_BLOCK];
	m_cache->read(blk, block);
	for (unsigned i = 0; i < m_descSize; i++) buf[i] = block[off + i];
}

void ExtAllocator::writeDesc(unsigned g, unsigned char* buf) {
	if (m_metaCsum) {
		wr16(buf, EXT_GD_CHECKSUM_OFFSET, 0);
		unsigned short c = extGroupDescCsum(m_seed, g, buf, m_descSize);
		wr16(buf, EXT_GD_CHECKSUM_OFFSET, c);
	}
	unsigned blk, off;
	descLoc(g, blk, off);
	m_cache->writePartial(blk, off, buf, m_descSize);
}

void ExtAllocator::writeSuperblock() {
	if (m_metaCsum)
		wr32(m_sb, EXT_SB_CHECKSUM, extSuperblockCsum(m_sb));
	// Primary superblock: block 1 for 1 KiB blocks, else bytes 1024.. of block 0.
	if (m_blockSize == 1024)
		m_cache->write(1, m_sb);
	else
		m_cache->writePartial(0, 1024, m_sb, 1024);
}

int ExtAllocator::bitmapAlloc(unsigned bitmapBlock, unsigned bits) {
	unsigned char bm[BlockCache::MAX_BLOCK];
	m_cache->read(bitmapBlock, bm);
	for (unsigned i = 0; i < bits; i++)
		if (!(bm[i >> 3] & (1 << (i & 7)))) {
			bm[i >> 3] |= (unsigned char) (1 << (i & 7));
			m_cache->write(bitmapBlock, bm);
			return (int) i;
		}
	return -1;
}

void ExtAllocator::bitmapBit(unsigned bitmapBlock, unsigned bit, bool set) {
	unsigned char bm[BlockCache::MAX_BLOCK];
	m_cache->read(bitmapBlock, bm);
	if (set) bm[bit >> 3] |= (unsigned char) (1 << (bit & 7));
	else     bm[bit >> 3] &= (unsigned char) ~(1 << (bit & 7));
	m_cache->write(bitmapBlock, bm);
}

void ExtAllocator::refreshBitmapCsum(unsigned char* desc, unsigned bitmapBlock, bool isBlockBitmap) {
	if (!m_metaCsum)
		return;
	unsigned char bm[BlockCache::MAX_BLOCK];
	m_cache->read(bitmapBlock, bm);
	// e2fsprogs checksums the block bitmap over (blocks_per_group+7)/8 bytes and the inode bitmap
	// over (inodes_per_group+7)/8 bytes — the inode bitmap is usually shorter than a full block.
	unsigned bits = isBlockBitmap ? m_blocksPerGroup : m_inodesPerGroup;
	unsigned numBytes = (bits + 7) / 8;
	unsigned c = extBitmapCsum(m_seed, bm, numBytes);
	if (isBlockBitmap) {
		descSetU16(desc, GD_BB_CSUM_LO, c & 0xFFFF);
		if (m_descSize >= 64) descSetU16(desc, GD_BB_CSUM_HI, (c >> 16) & 0xFFFF);
	} else {
		descSetU16(desc, GD_IB_CSUM_LO, c & 0xFFFF);
		if (m_descSize >= 64) descSetU16(desc, GD_IB_CSUM_HI, (c >> 16) & 0xFFFF);
	}
}

unsigned ExtAllocator::allocBlock(unsigned goalGroup) {
	for (unsigned n = 0; n < m_groupCount; n++) {
		unsigned g = (goalGroup + n) % m_groupCount;
		unsigned char desc[64];
		readDesc(g, desc);
		if (descCount(desc, GD_FREE_BLOCKS_LO, GD_FREE_BLOCKS_HI) == 0)
			continue;
		unsigned bb = descBitmapBlock(desc, GD_BLOCK_BITMAP_LO, GD_BLOCK_BITMAP_HI);
		int bit = bitmapAlloc(bb, m_blocksPerGroup);
		if (bit < 0)
			continue;                          // count said free but bitmap full -> next group
		descSetU16(desc, GD_FLAGS, descU16(desc, GD_FLAGS) & ~BG_BLOCK_UNINIT);
		refreshBitmapCsum(desc, bb, true);
		descSetCount(desc, GD_FREE_BLOCKS_LO, GD_FREE_BLOCKS_HI,
		             descCount(desc, GD_FREE_BLOCKS_LO, GD_FREE_BLOCKS_HI) - 1);
		writeDesc(g, desc);
		unsigned sbFree = sbU32(SB_FREE_BLOCKS_LO);   // low 32 bits suffice for our sizes
		sbSetU32(SB_FREE_BLOCKS_LO, sbFree - 1);
		writeSuperblock();
		return m_firstDataBlock + g * m_blocksPerGroup + (unsigned) bit;
	}
	return 0;
}

void ExtAllocator::freeBlock(unsigned blockNo) {
	if (blockNo < m_firstDataBlock)
		return;
	unsigned rel = blockNo - m_firstDataBlock;
	unsigned g = rel / m_blocksPerGroup;
	unsigned bit = rel % m_blocksPerGroup;
	if (g >= m_groupCount)
		return;
	unsigned char desc[64];
	readDesc(g, desc);
	unsigned bb = descBitmapBlock(desc, GD_BLOCK_BITMAP_LO, GD_BLOCK_BITMAP_HI);
	bitmapBit(bb, bit, false);
	refreshBitmapCsum(desc, bb, true);
	descSetCount(desc, GD_FREE_BLOCKS_LO, GD_FREE_BLOCKS_HI,
	             descCount(desc, GD_FREE_BLOCKS_LO, GD_FREE_BLOCKS_HI) + 1);
	writeDesc(g, desc);
	sbSetU32(SB_FREE_BLOCKS_LO, sbU32(SB_FREE_BLOCKS_LO) + 1);
	writeSuperblock();
}

unsigned ExtAllocator::allocInode(bool isDir, unsigned goalGroup) {
	for (unsigned n = 0; n < m_groupCount; n++) {
		unsigned g = (goalGroup + n) % m_groupCount;
		unsigned char desc[64];
		readDesc(g, desc);
		if (descCount(desc, GD_FREE_INODES_LO, GD_FREE_INODES_HI) == 0)
			continue;
		unsigned ib = descBitmapBlock(desc, GD_INODE_BITMAP_LO, GD_INODE_BITMAP_HI);
		int bit = bitmapAlloc(ib, m_inodesPerGroup);
		if (bit < 0)
			continue;
		descSetU16(desc, GD_FLAGS, descU16(desc, GD_FLAGS) & ~BG_INODE_UNINIT);
		refreshBitmapCsum(desc, ib, false);
		descSetCount(desc, GD_FREE_INODES_LO, GD_FREE_INODES_HI,
		             descCount(desc, GD_FREE_INODES_LO, GD_FREE_INODES_HI) - 1);
		if (isDir)
			descSetCount(desc, GD_USED_DIRS_LO, GD_USED_DIRS_HI,
			             descCount(desc, GD_USED_DIRS_LO, GD_USED_DIRS_HI) + 1);
		// itable_unused counts never-used inodes at the tail; shrink it if we used past it.
		unsigned unused = descCount(desc, GD_ITABLE_UNUSED_LO, GD_ITABLE_UNUSED_HI);
		unsigned tail = m_inodesPerGroup - (unsigned) (bit + 1);
		if (unused > tail)
			descSetCount(desc, GD_ITABLE_UNUSED_LO, GD_ITABLE_UNUSED_HI, tail);
		writeDesc(g, desc);
		sbSetU32(SB_FREE_INODES, sbU32(SB_FREE_INODES) - 1);
		writeSuperblock();
		return g * m_inodesPerGroup + (unsigned) bit + 1;
	}
	return 0;
}

void ExtAllocator::freeInode(unsigned inodeNo, bool isDir) {
	if (inodeNo == 0)
		return;
	unsigned idx = inodeNo - 1;
	unsigned g = idx / m_inodesPerGroup;
	unsigned bit = idx % m_inodesPerGroup;
	if (g >= m_groupCount)
		return;
	unsigned char desc[64];
	readDesc(g, desc);
	unsigned ib = descBitmapBlock(desc, GD_INODE_BITMAP_LO, GD_INODE_BITMAP_HI);
	bitmapBit(ib, bit, false);
	refreshBitmapCsum(desc, ib, false);
	descSetCount(desc, GD_FREE_INODES_LO, GD_FREE_INODES_HI,
	             descCount(desc, GD_FREE_INODES_LO, GD_FREE_INODES_HI) + 1);
	if (isDir) {
		unsigned ud = descCount(desc, GD_USED_DIRS_LO, GD_USED_DIRS_HI);
		if (ud > 0) descSetCount(desc, GD_USED_DIRS_LO, GD_USED_DIRS_HI, ud - 1);
	}
	writeDesc(g, desc);
	sbSetU32(SB_FREE_INODES, sbU32(SB_FREE_INODES) + 1);
	writeSuperblock();
}

void ExtAllocator::readInode(unsigned ino, unsigned char* out) {
	unsigned blk, off;
	inodeLocation(ino, blk, off);
	unsigned char block[BlockCache::MAX_BLOCK];
	m_cache->read(blk, block);
	for (unsigned i = 0; i < m_inodeSize; i++) out[i] = block[off + i];
}

void ExtAllocator::writeInode(unsigned ino, unsigned char* inode) {
	if (m_metaCsum)
		extInodeCsum(m_seed, ino, inode, m_inodeSize);   // recompute i_checksum_lo/hi in place
	unsigned blk, off;
	inodeLocation(ino, blk, off);
	m_cache->writePartial(blk, off, inode, m_inodeSize);
}

void ExtAllocator::inodeLocation(unsigned ino, unsigned& blockOut, unsigned& offsetOut) const {
	unsigned g = (ino - 1) / m_inodesPerGroup;
	unsigned idx = (ino - 1) % m_inodesPerGroup;
	unsigned char desc[64];
	const_cast<ExtAllocator*>(this)->readDesc(g, desc);
	unsigned itable = rd32(desc, 0x08);
	blockOut = itable + (idx * m_inodeSize) / m_blockSize;
	offsetOut = (idx * m_inodeSize) % m_blockSize;
}

}  // namespace kernel
