/*
 * Ext2Filesystem.h
 *
 * ext2 specialisation of the shared ExtFilesystem core: blocks are addressed by
 * the inode's direct block pointers (read-only, blocks 0-11).
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
	unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex) {
		return inode.directBlocks[fileBlockIndex];
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
