/*
 * Ext2Filesystem.h
 *
 * ext2 specialisation of the shared ExtFilesystem core: blocks are addressed by the
 * inode's 12 direct pointers plus the single/double/triple indirect block chains, so
 * files larger than 12 blocks are read correctly (not just the first 48 KiB).
 */
#include "ExtFilesystem.h"
#ifndef EXT2FILESYSTEM_H_
#define EXT2FILESYSTEM_H_

namespace kernel {

class Ext2Filesystem: public ExtFilesystem {
	// Read pointer-block `blk` and return its `idx`-th 4-byte block pointer (0 = hole).
	// Uses the shared commonBuff; safe because readFile consumes resolveBlock's return
	// value (a block number) and then re-reads commonBuff with the data block.
	unsigned blockPtr(unsigned blk, unsigned idx) {
		if (blk == 0)
			return 0;
		cache->read(blk, commonBuff);
		return (unsigned) ((unsigned*) commonBuff)[idx];
	}
public:
	Ext2Filesystem(BlockDevice* device, unsigned partitionLba) :
			ExtFilesystem(device, partitionLba) {
	}
	// Map a file block index to an absolute fs block via direct + indirect pointers.
	unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex) {
		unsigned k = (unsigned) blockSize / 4u;     // block pointers per indirect block
		if (fileBlockIndex < 12)
			return (unsigned) inode.directBlocks[fileBlockIndex];
		fileBlockIndex -= 12;
		if (fileBlockIndex < k)                     // single indirect
			return blockPtr((unsigned) inode.indirectPtr, fileBlockIndex);
		fileBlockIndex -= k;
		if (fileBlockIndex < k * k) {               // double indirect
			unsigned mid = blockPtr((unsigned) inode.doubleIndirectPtr, fileBlockIndex / k);
			return blockPtr(mid, fileBlockIndex % k);
		}
		fileBlockIndex -= k * k;                     // triple indirect
		unsigned hi  = blockPtr((unsigned) inode.tripleIndirectPtr, fileBlockIndex / (k * k));
		unsigned mid = blockPtr(hi, (fileBlockIndex / k) % k);
		return blockPtr(mid, fileBlockIndex % k);
	}
};

// Factory + probe so the VFS can mount "ext2" (or pick it via auto-detect).
class Ext2FileSystemType: public FileSystemType {
public:
	const char* name() {
		return "ext2";
	}
	bool probe(BlockDevice* dev, unsigned partitionLba) {
		char sb[1024];
		dev->readSectors(partitionLba + 2, 2, sb);
		unsigned short magic = *(unsigned short*) (sb + 0x38);
		unsigned incompat = *(unsigned*) (sb + 0x60);
		// ext2: valid magic and none of the ext4 incompat features.
		return magic == 0xEF53 && !(incompat & (0x40 | 0x80));
	}
	FileSystem* create(BlockDevice* dev, unsigned partitionLba) {
		return new Ext2Filesystem(dev, partitionLba);
	}
};

} /* namespace kernel */

#endif /* EXT2FILESYSTEM_H_ */
