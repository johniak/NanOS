/*
 * Ext2Filesystem.h
 *
 *  Created on: Feb 4, 2014
 *      Author: johniak
 */

#include "Hdd.h"
#include "Console.h"
#include "string.h"
#include "List.h"
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
	int dbp0;
	int dbp1;
	int dbp2;
	int dbp3;
	int dbp4;
	int dbp5;
	int dbp6;
	int dbp7;
	int dbp8;
	int dbp9;
	int dbp10;
	int dbp11;
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



class Ext2Filesystem {
	char superblockBuff[1024];
	char commonBuff[4096];
	Ext2BaseSuperblockFields baseSuperBlock;
	Ext2ExtendedSuperblockFields extendedSuperblock;
	Ext2BlockGroupDescriptor blockGroupDescriptors[100];
	int blockGroupsCount;
	int partitionLba;
	int BgdtSector;
	int blockSize;
public:
	void initialize(int partitionLba) {
		this->partitionLba = partitionLba;
		Hdd::readSectors(this->partitionLba + 2, 2, superblockBuff);
		memcpy((void*) &baseSuperBlock, (void*) superblockBuff,
				sizeof(Ext2BaseSuperblockFields));
		memcpy((void*) &extendedSuperblock, (void*) superblockBuff,
				sizeof(Ext2ExtendedSuperblockFields));
		initBgdt();
		printInfo();

//		for (int i = 1; i < 6; i++) {
//			Ext2Inode inode = readInode(i);
//			Console::writeLine(inode.dbp0);
//			Console::writeHex(inode.typeAndPermisions);
//			Console::writeLine("");
//		}
		Ext2Inode inode = readInode(2);
		List<Ext2DirectoryEntry> dirs= getDirectoriesEntries(inode);
//		Console::write("dir len: ");
//		Console::writeLine((int) dirs.entries[0].nameLowLenght);
//		Console::writeLine( dirs.entries[0].name);
//		Console::write("dir len: ");
//		Console::writeLine((int) dirs.entries[1].nameLowLenght);
		for(int i=0;i<dirs.getCount();i++)
			Console::writeLine( dirs[i].name);

	}
	void initBgdt() {
		blockGroupsCount = (int) ceil(
				((float) baseSuperBlock.totalBlocks)
						/ ((float) baseSuperBlock.blockInGroup));
		blockSize = 1024 << baseSuperBlock.log2BlockSize;

		int sectorCount = ceil(
				((float) (blockGroupsCount * sizeof(Ext2BlockGroupDescriptor)))
						/ 512.0);
		Hdd::readSectors(this->partitionLba + 2 + 2, sectorCount, commonBuff);
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
		;
	}
	Ext2Inode readInode(int inodeNumber) {
		int blockGroupNumber = (inodeNumber - 1) / baseSuperBlock.inodesInGroup;
		int inodeIndex = (inodeNumber - 1) % baseSuperBlock.inodesInGroup;
		Ext2Inode inode;
		int blockAddress =
				blockGroupDescriptors[blockGroupNumber].StartingBlockAddressOfInodeTable
						+ ((inodeIndex * 128) / blockSize);
		int blockOffset = (inodeIndex * 128) % blockSize;
		Console::write("inode block: ");
		Console::writeLine(blockAddress);
		Console::write("inode block offset: ");
		Console::writeLine(blockOffset);
		Hdd::readSectors(this->partitionLba + blockAddress * (blockSize / 512),
				1, commonBuff);
		memcpy(&inode, commonBuff + blockOffset, 128);
		return inode;
	}
	int getFileType(short typeAndPermisions) {
		int type = 0;
		type = (int) typeAndPermisions & 0x0000F000;
		return type;
	}
	Ext2DirectoryEntry getDirectoryEntry(int block) {
		Ext2DirectoryEntry dir;
		Hdd::readSectors(this->partitionLba + block * (blockSize / 512), 1,
				commonBuff);

		memcpy(&dir, commonBuff, sizeof(dir) - 256);
		memcpy(&dir.name, commonBuff + sizeof(dir) - 256, dir.nameLowLenght);
		dir.name[255] = 0;
		return dir;
	}

	List<Ext2DirectoryEntry> getDirectoriesEntries(Ext2Inode inode) {
		int block = inode.dbp0;
		int index = 0;
		int offset = 0;
		List<Ext2DirectoryEntry> dirs;
		Hdd::readSectors(this->partitionLba + block * (blockSize / 512), 1,
				commonBuff);
		while (offset < inode.lowerSize) {
			dirs.add(Ext2DirectoryEntry());
			memcpy(&dirs[index], commonBuff+offset,
					sizeof(Ext2DirectoryEntry) - 256);
			int tmp= offset + sizeof(Ext2DirectoryEntry) - 256;

			memcpy(&dirs[index].name, commonBuff + tmp, dirs[index].nameLowLenght);
			offset+=dirs[index].entrySize;
			index++;
			Console::writeLine(inode.lowerSize);
		}
		return dirs;
	}
};

} /* namespace kernel */

#endif /* EXT2FILESYSTEM_H_ */
