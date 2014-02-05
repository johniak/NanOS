/*
 * Ext2Filesystem.h
 *
 *  Created on: Feb 4, 2014
 *      Author: johniak
 */

#include "Hdd.h"
#include "Console.h"
#include "string.h"
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

class Ext2Filesystem {
	char superblockBuff[100];
	Ext2BaseSuperblockFields baseSuperBlock;
	Ext2ExtendedSuperblockFields extendedSuperblock;
	Ext2BlockGroupDescriptor blockGroupDescriptor[256];
	int blockGroupsCount;
	int partitionLba;
	int BgdtSector;
public:
	void initialize(int partitionLba) {
		this->partitionLba = partitionLba;
		Hdd::readSectors(this->partitionLba + 2, 2, superblockBuff);
		memcpy((void*) &baseSuperBlock, (void*) superblockBuff, sizeof(Ext2BaseSuperblockFields));
		memcpy((void*) &extendedSuperblock, (void*) superblockBuff, sizeof(Ext2ExtendedSuperblockFields));
		blockGroupsCount= (int)ceil(((float)baseSuperBlock.totalBlocks)/((float)baseSuperBlock.blockInGroup));
		printInfo();
	}
	void initBgdt(){

	}
	void printInfo(){
		int blockSize =baseSuperBlock.log2BlockSize<<10;
		Console::write("Block size: ");
		Console::writeLine(blockSize);
		Console::write("Total inodes: ");
		Console::writeLine(baseSuperBlock.totalInodes);
		Console::write("Total blocks: ");
		Console::writeLine(baseSuperBlock.totalBlocks);
		Console::write("Total blocksGroups: ");
		Console::writeLine(blockGroupsCount);
	}
};

} /* namespace kernel */

#endif /* EXT2FILESYSTEM_H_ */
