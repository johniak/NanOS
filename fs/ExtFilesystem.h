/*
 * ExtFilesystem.h
 *
 * Shared core for the ext2/ext3/ext4 family: superblock, block group
 * descriptors, inode and directory parsing, and the read/stat/readdir
 * machinery. The single point of variation between ext2 and ext4 is mapping a
 * file-relative block index to an absolute filesystem block — exposed as the
 * virtual resolveBlock(). Subclasses implement only that.
 */
#include "Vfs.h"
#include "Console.h"
#include "string.h"
#include "List.h"
#include "String.h"
#ifndef EXTFILESYSTEM_H_
#define EXTFILESYSTEM_H_

namespace kernel {
int ceil(float num);

struct Ext2BaseSuperblockFields {
	int totalInodes;
	int totalBlocks;
	int superuserBlocks;
	int unallocatedBlocks;
	int unallocatedInodes;
	int blockOfSuperblock;
	int log2BlockSize;
	int log2FragmentSize;
	int blockInGroup;
	int fragmentInGroup;
	int inodesInGroup;
	int lastMountTime;
	int lastWriteTime;
	short mountCountSinceFsck;
	short mountCountAllowedBeforeFsck;
	short signature;
	short state;
	short errorWay;
	short minorVersion;
	int lastFsckTime;
	int interwalBetweenFsck;
	int osID;
	int majorVersion;
	short ownerOfReservedBlocks;
	short ownersGroupOfReservedBlocks;
};

struct Ext2ExtendedSuperblockFields {
	int firstNotReservedInode;
	short sizeOfInodeStructure;
	short bolckGroupOwnerOfSuperblock;
	int optionalsFeatures;
	int requairedFeatures;
	int featuresSuportedForReadOnly;
	int fsID[4];
	char volumeName[16];
	char lastMountedPath[64];
	int compressionAlgorithm;
	char prelocatedBlocksForFiles;
	char prelocatedBlocksForDirectories;
	short unused;
	int journalID[4];
	int inode;
	int journalDevice;
};

struct Ext2BlockGroupDescriptor {
	int BlockAddressOfBlockUsageBitmap;
	int BlockAddressOfInodeUsageBitmap;
	int StartingBlockAddressOfInodeTable;
	short NumberOfUnallocatedBlocksInGroup;
	short NumberOfUnallocatedInodesInGroup;
	short NumberOfDirectoriesInGroup;
	char unused[14];
};

struct Ext2Inode {
	short typeAndPermisions;
	short userId;
	int lowerSize;
	int lastAccess;
	int created;
	int lastmodification;
	int deleted;
	short groupId;
	short hardlinksCount;
	int sectorsCount;
	int flags;
	int osSpecific;
	int directBlocks[12];
	int indirectPtr;
	int doubleIndirectPtr;
	int tripleIndirectPtr;
	int generationNumber;
	int ExtBlockAttr;
	int upperSize;
	int fragmentAddr;
	char osSpecific2[12];
};

struct Ext2DirectoryEntry {
	int inode;
	short entrySize;
	char nameLowLenght;
	char typeindicator;
	char name[256];
};

class ExtFilesystem: public FileSystem {
protected:
	BlockDevice* device;
	char superblockBuff[1024];
	char commonBuff[4096];
	Ext2BaseSuperblockFields baseSuperBlock;
	Ext2ExtendedSuperblockFields extendedSuperblock;
	Ext2BlockGroupDescriptor blockGroupDescriptors[100];
	int blockGroupsCount;
	int partitionLba;
	int blockSize;
	int inodeSize;
	int descSize;
public:
	ExtFilesystem(BlockDevice* device, unsigned partitionLba) {
		this->device = device;
		this->partitionLba = partitionLba;
	}

	// Map a file-relative block index to an absolute filesystem block number.
	// ext2 uses direct pointers; ext4 walks an extent tree.
	virtual unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex) = 0;

	int mount() {
		device->readSectors(this->partitionLba + 2, 2, superblockBuff);
		memcpy((void*) &baseSuperBlock, (void*) superblockBuff,
				sizeof(Ext2BaseSuperblockFields));
		// The extended superblock fields follow the base fields, not at offset 0.
		memcpy((void*) &extendedSuperblock,
				(void*) (superblockBuff + sizeof(Ext2BaseSuperblockFields)),
				sizeof(Ext2ExtendedSuperblockFields));
		// Inode size: 128 for ext2 rev 0; recorded in the superblock for rev >= 1
		// (modern mke2fs defaults to 256).
		inodeSize = extendedSuperblock.sizeOfInodeStructure;
		if (inodeSize <= 0)
			inodeSize = 128;
		// 64-bit feature -> 64-byte group descriptors (s_desc_size @ byte 0xFE).
		descSize = 32;
		unsigned incompat = *(unsigned*) (superblockBuff + 0x60);
		if (incompat & 0x80) {
			unsigned short ds = *(unsigned short*) (superblockBuff + 0xFE);
			if (ds >= 32)
				descSize = ds;
		}
		initBgdt();
		printInfo();
		return 0;
	}

	void initBgdt() {
		blockGroupsCount = (int) ceil(
				((float) baseSuperBlock.totalBlocks)
						/ ((float) baseSuperBlock.blockInGroup));
		blockSize = 1024 << baseSuperBlock.log2BlockSize;
		// The group descriptor table starts in the block after the superblock:
		// block 2 for 1 KiB blocks, block 1 otherwise.
		int bgdtBlock = (blockSize == 1024) ? 2 : 1;
		int sectorCount = (blockGroupsCount * descSize + 511) / 512 + 1;
		device->readSectors(this->partitionLba + bgdtBlock * (blockSize / 512),
				sectorCount, commonBuff);
		for (int g = 0; g < blockGroupsCount; g++)
			memcpy((void*) &blockGroupDescriptors[g],
					(void*) (commonBuff + g * descSize),
					sizeof(Ext2BlockGroupDescriptor));
	}

	void printInfo() {
		Console::write("Block size: ");
		Console::writeLine(blockSize);
		Console::write("Total inodes: ");
		Console::writeLine(baseSuperBlock.totalInodes);
		Console::write("Total blocks: ");
		Console::writeLine(baseSuperBlock.totalBlocks);
		Console::write("Total blocksGroups: ");
		Console::writeLine(blockGroupsCount);
		Console::writeLine("Extfs initialized");
	}

	Ext2Inode getInode(int inodeNumber) {
		int blockGroupNumber = (inodeNumber - 1) / baseSuperBlock.inodesInGroup;
		int inodeIndex = (inodeNumber - 1) % baseSuperBlock.inodesInGroup;
		Ext2Inode inode;
		int blockAddress =
				blockGroupDescriptors[blockGroupNumber].StartingBlockAddressOfInodeTable
						+ ((inodeIndex * inodeSize) / blockSize);
		int blockOffset = (inodeIndex * inodeSize) % blockSize;
		device->readSectors(this->partitionLba + blockAddress * (blockSize / 512),
				2, commonBuff);
		memcpy(&inode, commonBuff + blockOffset, 128);
		return inode;
	}

	List<Ext2DirectoryEntry> getDirectoriesEntries(Ext2Inode inode) {
		List<Ext2DirectoryEntry> dirs;
		unsigned size = (unsigned) inode.lowerSize;
		unsigned nblocks = (size + blockSize - 1) / blockSize;
		const int HDR = sizeof(Ext2DirectoryEntry) - 256;   // fixed header before name[]
		// Directories can span several blocks; entries never cross a block boundary
		// (the last in a block pads to its end). Walk block by block.
		for (unsigned fb = 0; fb < nblocks; fb++) {
			unsigned block = resolveBlock(inode, fb);
			if (block == 0)
				continue;
			device->readSectors(this->partitionLba + block * (blockSize / 512),
					(blockSize / 512), commonBuff);
			int off = 0;
			while (off + HDR <= blockSize) {
				Ext2DirectoryEntry de;
				memcpy(&de, commonBuff + off, HDR);
				if (de.entrySize <= 0)
					break;                  // malformed/zero rec_len -> stop (no infinite loop)
				int nl = (int) (unsigned char) de.nameLowLenght;
				if (nl > 255)
					nl = 255;
				memcpy(de.name, commonBuff + off + HDR, nl);
				de.name[nl] = 0;
				if (de.inode != 0)          // skip deleted / unused slots
					dirs.add(de);
				off += de.entrySize;
			}
		}
		return dirs;
	}

	// Resolve an absolute path to its inode. Returns false if any component is
	// missing.
	bool getInodeByPath(String path, Ext2Inode& out) {
		if (!path.startsWith("/"))
			return false;
		path = path.substring(1);
		List<String> pathSplited = path.split('/');
		Ext2Inode parentInode = getInode(2);
		for (int i = 0; i < pathSplited.getCount(); i++) {
			if (pathSplited[i].getLenght() == 0)
				continue;
			Ext2Inode childInode;
			if (!getChildrenInode(parentInode, pathSplited[i], childInode))
				return false;
			parentInode = childInode;
		}
		out = parentInode;
		return true;
	}

	bool getChildrenInode(Ext2Inode inode, String name, Ext2Inode& out) {
		if (!isDirectory(inode))
			return false;
		List<Ext2DirectoryEntry> entries = getDirectoriesEntries(inode);
		for (int i = 0; i < entries.getCount(); i++) {
			if (name.compareTo(entries[i].name) == 0) {
				out = getInode(entries[i].inode);
				return true;
			}
		}
		return false;
	}

	bool isDirectory(Ext2Inode inode) {
		return (inode.typeAndPermisions & 0xF000) == 0x4000;
	}

	void ls(String path) {
		Ext2Inode inode;
		if (!getInodeByPath(path, inode)) {
			Console::writeLine("ERROR :: Wrong path");
			return;
		}
		if (!isDirectory(inode)) {
			Console::writeLine("ERROR :: Path does not lead to the directory");
			return;
		}
		List<Ext2DirectoryEntry> entries = getDirectoriesEntries(inode);
		for (int i = 0; i < entries.getCount(); i++)
			Console::writeLine(entries[i].name);
	}

	// ---- VFS FileSystem interface ----
	int read(String path, unsigned size, unsigned offset, void* buff) {
		Ext2Inode inode;
		if (!getInodeByPath(path, inode))
			return -1;
		return readFile(inode, size, offset, buff);
	}

	int stat(String path, FileStat& out) {
		Ext2Inode inode;
		if (!getInodeByPath(path, inode))
			return -1;
		out.type = isDirectory(inode) ? NODE_DIR : NODE_FILE;
		out.size = inode.lowerSize;
		out.mode = (unsigned) (unsigned short) inode.typeAndPermisions;
		out.nlink = (unsigned) (unsigned short) inode.hardlinksCount;
		out.uid = (unsigned) (unsigned short) inode.userId;
		out.gid = (unsigned) (unsigned short) inode.groupId;
		out.mtime = (unsigned) inode.lastmodification;
		return 0;
	}

	int readdir(String path, List<DirEntry>& out) {
		Ext2Inode inode;
		if (!getInodeByPath(path, inode))
			return -1;
		if (!isDirectory(inode))
			return -1;
		List<Ext2DirectoryEntry> entries = getDirectoriesEntries(inode);
		for (int i = 0; i < entries.getCount(); i++) {
			DirEntry de;
			int n = (int) (unsigned char) entries[i].nameLowLenght;
			if (n > 255)
				n = 255;
			for (int k = 0; k < n; k++)
				de.name[k] = entries[i].name[k];
			de.name[n] = 0;
			de.type = entries[i].typeindicator == 2 ? NODE_DIR :
					entries[i].typeindicator == 1 ? NODE_FILE : NODE_OTHER;
			out.add(de);
		}
		return 0;
	}

	// Read [offset, offset+size) of a file, block by block via resolveBlock().
	int readFile(Ext2Inode inode, unsigned size, unsigned offset, void* buff) {
		unsigned fileSize = (unsigned) inode.lowerSize;
		if (offset >= fileSize)
			return -1;
		if (offset + size > fileSize)
			size = fileSize - offset;
		char* out = (char*) buff;
		unsigned written = 0;
		while (written < size) {
			unsigned filePos = offset + written;
			unsigned blockIndex = filePos / blockSize;
			unsigned within = filePos % blockSize;
			unsigned chunk = blockSize - within;
			if (chunk > size - written)
				chunk = size - written;
			unsigned blockAddress = resolveBlock(inode, blockIndex);
			device->readSectors(
					this->partitionLba + blockAddress * (blockSize / 512),
					(blockSize / 512), commonBuff);
			memcpy(out + written, commonBuff + within, chunk);
			written += chunk;
		}
		return size;
	}
};

} /* namespace kernel */

#endif /* EXTFILESYSTEM_H_ */
