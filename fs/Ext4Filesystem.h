/*
 * Ext4Filesystem.h
 *
 * ext4 specialisation of the shared ExtFilesystem core. Differs from ext2 only
 * in block mapping: ext4 inodes carry an extent tree in the i_block area
 * instead of direct/indirect pointers. The 64-bit descriptor size is handled by
 * the core (ExtFilesystem::mount reads s_desc_size).
 */
#include "ExtFilesystem.h"
#ifndef EXT4FILESYSTEM_H_
#define EXT4FILESYSTEM_H_

namespace kernel {

struct Ext4ExtentHeader {
	unsigned short magic;     // 0xF30A
	unsigned short entries;
	unsigned short max;
	unsigned short depth;     // 0 = leaf (extents), >0 = index nodes
	unsigned generation;
};
struct Ext4ExtentIdx {        // interior node entry
	unsigned fileBlock;
	unsigned leafLo;
	unsigned short leafHi;
	unsigned short unused;
};
struct Ext4Extent {           // leaf entry
	unsigned fileBlock;
	unsigned short len;
	unsigned short startHi;
	unsigned startLo;
};

class Ext4Filesystem: public ExtFilesystem {
	char extentBuf[4096];
public:
	Ext4Filesystem(BlockDevice* device, unsigned partitionLba) :
			ExtFilesystem(device, partitionLba) {
	}

	unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex) {
		// Inodes without the extents flag use classic direct blocks.
		if (!(inode.flags & 0x80000))
			return inode.directBlocks[fileBlockIndex];

		// The 60-byte i_block area starts at directBlocks[0]: an extent header
		// followed by up to 4 entries. Copy it out, then walk the tree (reading
		// index/leaf blocks into extentBuf as we descend).
		char node[60];
		memcpy(node, &inode.directBlocks[0], 60);
		char* cur = node;
		for (;;) {
			Ext4ExtentHeader* h = (Ext4ExtentHeader*) cur;
			if (h->magic != 0xF30A)
				return 0;
			if (h->depth == 0) {
				Ext4Extent* ex = (Ext4Extent*) (cur + sizeof(Ext4ExtentHeader));
				for (int i = 0; i < h->entries; i++) {
					unsigned start = ex[i].fileBlock;
					unsigned end = start + ex[i].len;
					if (fileBlockIndex >= start && fileBlockIndex < end)
						return ex[i].startLo + (fileBlockIndex - start);
				}
				return 0;
			}
			// Interior node: descend into the index covering fileBlockIndex.
			Ext4ExtentIdx* ix = (Ext4ExtentIdx*) (cur + sizeof(Ext4ExtentHeader));
			int pick = 0;
			for (int i = 0; i < h->entries; i++)
				if (ix[i].fileBlock <= fileBlockIndex)
					pick = i;
			unsigned child = ix[pick].leafLo;
			device->readSectors(this->partitionLba + child * (blockSize / 512),
					(blockSize / 512), extentBuf);
			cur = extentBuf;
		}
	}
};

// Factory + probe so the VFS can mount "ext4" (or pick it via auto-detect).
class Ext4FileSystemType: public FileSystemType {
public:
	const char* name() {
		return "ext4";
	}
	bool probe(BlockDevice* dev, unsigned partitionLba) {
		char sb[1024];
		dev->readSectors(partitionLba + 2, 2, sb);
		unsigned short magic = *(unsigned short*) (sb + 0x38);
		unsigned incompat = *(unsigned*) (sb + 0x60);
		// ext4: valid magic and at least one of the extents/64-bit features.
		return magic == 0xEF53 && (incompat & (0x40 | 0x80));
	}
	FileSystem* create(BlockDevice* dev, unsigned partitionLba) {
		return new Ext4Filesystem(dev, partitionLba);
	}
};

} /* namespace kernel */

#endif /* EXT4FILESYSTEM_H_ */
