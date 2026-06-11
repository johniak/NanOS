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
public:
	Ext2Filesystem(BlockDevice* device, unsigned partitionLba) :
			ExtFilesystem(device, partitionLba) {
	}
	// ext2 maps blocks classically; the direct/indirect machinery is shared in the core.
	unsigned resolveBlock(Ext2Inode& inode, unsigned fb) { return resolveIndirect(inode, fb); }
	unsigned bmapAlloc(Ext2Inode& inode, unsigned inodeNo, unsigned fb) {
		return bmapAllocIndirect(inode, inodeNo, fb);
	}
	void truncateBlocks(Ext2Inode& inode, unsigned inodeNo, unsigned firstFree) {
		(void) inodeNo;
		truncateIndirect(inode, firstFree);
	}

	void initInodeBlockmap(Ext2Inode& inode, bool isDir) {
		(void) isDir;
		for (int i = 0; i < 12; i++)
			inode.directBlocks[i] = 0;
		inode.indirectPtr = 0;
		inode.doubleIndirectPtr = 0;
		inode.tripleIndirectPtr = 0;
		inode.flags &= ~0x80000;          // ext2 has no extents
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
