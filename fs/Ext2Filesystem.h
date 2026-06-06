/*
 * Ext2Filesystem.h
 *
 *  Created on: Feb 4, 2014
 *      Author: johniak
 */

#include "Vfs.h"
#include "Console.h"
#include "string.h"
#include "List.h"
#include "String.h"
#ifndef EXT2FILESYSTEM_H_
#define EXT2FILESYSTEM_H_

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

class Ext2Filesystem: public FileSystem {
	BlockDevice* device;
	char superblockBuff[1024];
	char commonBuff[4096];
	Ext2BaseSuperblockFields baseSuperBlock;
	Ext2ExtendedSuperblockFields extendedSuperblock;
	Ext2BlockGroupDescriptor blockGroupDescriptors[100];
	int blockGroupsCount;
	int partitionLba;
	int BgdtSector;
	int blockSize;
	int inodeSize;
	char textBuf[128];
public:
	Ext2Filesystem(BlockDevice* device, unsigned partitionLba) {
		this->device = device;
		this->partitionLba = partitionLba;
	}
	int mount() {
		device->readSectors(this->partitionLba + 2, 2, superblockBuff);
		memcpy((void*) &baseSuperBlock, (void*) superblockBuff,
				sizeof(Ext2BaseSuperblockFields));
		// The extended superblock fields follow the base fields, not at offset 0.
		memcpy((void*) &extendedSuperblock,
				(void*) (superblockBuff + sizeof(Ext2BaseSuperblockFields)),
				sizeof(Ext2ExtendedSuperblockFields));
		// Inode size is 128 for ext2 rev 0; rev >= 1 records it in the superblock
		// (modern mke2fs defaults to 256). Indexing the inode table with a wrong
		// size reads garbage.
		inodeSize = extendedSuperblock.sizeOfInodeStructure;
		if (inodeSize <= 0)
			inodeSize = 128;
		initBgdt();
		printInfo();
		return 0;
	}
	void initBgdt() {
		blockGroupsCount = (int) ceil(
				((float) baseSuperBlock.totalBlocks)
						/ ((float) baseSuperBlock.blockInGroup));
		blockSize = 1024 << baseSuperBlock.log2BlockSize;

		int sectorCount = ceil(
				((float) (blockGroupsCount * sizeof(Ext2BlockGroupDescriptor)))
						/ 512.0);
		device->readSectors(this->partitionLba + 2 + 2, sectorCount, commonBuff);
		memcpy((void*) &blockGroupDescriptors, (void*) commonBuff,
				sizeof(Ext2BlockGroupDescriptor) * blockGroupsCount);
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
		Console::writeLine("Ext2fs initialized");
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
	int getFileType(short typeAndPermisions) {
		int type = 0;
		type = (int) typeAndPermisions & 0x0000F000;
		return type;
	}

	List<Ext2DirectoryEntry> getDirectoriesEntries(Ext2Inode inode) {
		int block = inode.directBlocks[0];
		int index = 0;
		int offset = 0;
		List<Ext2DirectoryEntry> dirs;
		device->readSectors(this->partitionLba + block * (blockSize / 512), 1,
				commonBuff);
		while (offset < inode.lowerSize) {
			dirs.add(Ext2DirectoryEntry());
			memcpy(&dirs[index], commonBuff + offset,
					sizeof(Ext2DirectoryEntry) - 256);
			int tmp = offset + sizeof(Ext2DirectoryEntry) - 256;

			memcpy(&dirs[index].name, commonBuff + tmp,
					dirs[index].nameLowLenght);
			offset += dirs[index].entrySize;
			index++;
			//Console::writeLine(inode.lowerSize);
		}
		return dirs;
	}
	// Resolve an absolute path to its inode. Returns false if any component is
	// missing. (Returns by value via out-param: the old version returned the
	// address of a local, a use-after-free.)
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
		for (int i = 0; i < entries.getCount(); i++) {
			Console::writeLine(entries[i].name);
		}
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

	int readFile(Ext2Inode inode, unsigned size, unsigned offset, void* buff) {
		if (offset >= inode.lowerSize) {
			return -1;
		}
		if (offset + size > inode.lowerSize) {
			size = inode.lowerSize - offset;
		}

		unsigned startBlock = offset / blockSize;
		unsigned offsetStartBlock = offset % blockSize;
		unsigned endBlock = (offset + size) / (blockSize + 1);
		unsigned sizeInEndBlock = (offset + size) % (blockSize + 1);
		unsigned directBlockEnd = endBlock > 11 ? 11 : endBlock;
		for (unsigned i = startBlock; i < directBlockEnd + 1; i++) {
			unsigned blockAddress = inode.directBlocks[i];
			device->readSectors(
					this->partitionLba + blockAddress * (blockSize / 512),
					(blockSize / 512), commonBuff);
			if (i == startBlock && i == endBlock) {
				memcpy(buff, commonBuff + offsetStartBlock, size);
			} else if (i == startBlock) {
				memcpy(buff, commonBuff + offsetStartBlock,
						blockSize - offsetStartBlock);
			} else if (i == endBlock) {
				int startInBuf = blockSize - offset;
				startInBuf += (i - startBlock - 2) * blockSize;
				memcpy(buff + startInBuf, commonBuff, sizeInEndBlock);
			} else {
				int startInBuf = blockSize - offset;
				startInBuf += (i - startBlock - 2) * blockSize;
				memcpy(buff + startInBuf, commonBuff, blockSize);
			}
		}



//		unsigned indirect = inode.indirectPtr;
//		device->readSectors(
//							this->partitionLba + indirect * (blockSize / 512),
//							(blockSize / 512), commonBuff);
//		int* ttt=(int*)commonBuff;
//		for(int i=0;i<10;i++){
//			Console::writeLine(ttt[i]);
//		}
		return size;
	}
};

// Factory registered with the VFS so "ext2" can be mounted on any BlockDevice.
class Ext2FileSystemType: public FileSystemType {
public:
	const char* name() {
		return "ext2";
	}
	FileSystem* create(BlockDevice* dev, unsigned partitionLba) {
		return new Ext2Filesystem(dev, partitionLba);
	}
};

} /* namespace kernel */

#endif /* EXT2FILESYSTEM_H_ */
