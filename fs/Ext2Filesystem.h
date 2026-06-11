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

	// Ensure a top-level indirect slot names an allocated, zeroed pointer block.
	unsigned ensureSlotBlock(int& slot, unsigned goal, Ext2Inode& inode) {
		if (slot)
			return (unsigned) slot;
		unsigned nb = alloc->allocBlock(goal);
		if (!nb)
			return 0;
		zeroBlock(nb);
		slot = (int) nb;
		addBlocksToInode(inode, 1);
		return nb;
	}

	// Ensure entry `idx` of pointer block `pb` names an allocated, zeroed block (data or a
	// deeper pointer block — both are zeroed: data for partial-write safety, pointer blocks so
	// their entries read as holes). Writes the updated entry back. Returns the block (0 = full).
	unsigned ensureEntry(unsigned pb, unsigned idx, unsigned goal, Ext2Inode& inode) {
		unsigned char buf[4096];
		cache->read(pb, buf);
		unsigned cur = ((unsigned*) buf)[idx];
		if (cur)
			return cur;
		unsigned nb = alloc->allocBlock(goal);   // touches bitmaps only — `buf` copy stays valid
		if (!nb)
			return 0;
		zeroBlock(nb);
		((unsigned*) buf)[idx] = nb;
		cache->write(pb, buf);
		addBlocksToInode(inode, 1);
		return nb;
	}

	// Free everything reachable from level-`level` pointer block `pb` (entry 0 maps file-block
	// `base`) for file-block indices >= firstFree. Returns true if `pb` becomes fully empty.
	bool freePtrTree(unsigned pb, int level, unsigned base, unsigned firstFree, Ext2Inode& inode) {
		unsigned k = (unsigned) blockSize / 4u;
		unsigned* e = (unsigned*) malloc((unsigned) blockSize);
		cache->read(pb, e);
		unsigned span = (level == 1) ? 1u : (level == 2) ? k : k * k;
		bool dirty = false, anyLeft = false;
		for (unsigned i = 0; i < k; i++) {
			if (e[i] == 0)
				continue;
			unsigned childBase = base + i * span;
			if (childBase + span <= firstFree) {        // wholly below the cut: keep
				anyLeft = true;
				continue;
			}
			if (level == 1) {
				if (childBase >= firstFree) {
					alloc->freeBlock(e[i]); e[i] = 0; dirty = true; addBlocksToInode(inode, -1);
				} else
					anyLeft = true;
			} else {
				if (freePtrTree(e[i], level - 1, childBase, firstFree, inode)) {
					alloc->freeBlock(e[i]); e[i] = 0; dirty = true; addBlocksToInode(inode, -1);
				} else
					anyLeft = true;
			}
		}
		if (dirty)
			cache->write(pb, e);
		free(e);
		return !anyLeft;
	}

	// Free a top-level indirect chain (and the top block itself when it empties), clearing the
	// inode pointer. `base` is the first file-block the chain maps; `level` 1/2/3.
	void freeTopIndirect(int& slot, int level, unsigned base, unsigned firstFree, Ext2Inode& inode) {
		if (slot == 0)
			return;
		unsigned k = (unsigned) blockSize / 4u;
		unsigned span = (level == 1) ? k : (level == 2) ? k * k : k * k * k;
		if (base + span <= firstFree)
			return;                                      // wholly kept
		bool emptied = freePtrTree((unsigned) slot, level, base, firstFree, inode);
		if (emptied) {
			alloc->freeBlock((unsigned) slot);
			addBlocksToInode(inode, -1);
			slot = 0;
		}
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

	// Allocate+map the block for `fileBlockIndex` (or return the existing one), growing the
	// direct/indirect chains as needed. Mirrors resolveBlock's index arithmetic.
	unsigned bmapAlloc(Ext2Inode& inode, unsigned inodeNo, unsigned fb) {
		unsigned existing = resolveBlock(inode, fb);
		if (existing)
			return existing;
		unsigned k = (unsigned) blockSize / 4u;
		unsigned goal = (inodeNo - 1) / (unsigned) baseSuperBlock.inodesInGroup;
		if (fb < 12) {
			unsigned nb = alloc->allocBlock(goal);
			if (!nb)
				return 0;
			zeroBlock(nb);
			inode.directBlocks[fb] = (int) nb;
			addBlocksToInode(inode, 1);
			return nb;
		}
		fb -= 12;
		if (fb < k) {
			unsigned ind = ensureSlotBlock(inode.indirectPtr, goal, inode);
			if (!ind)
				return 0;
			return ensureEntry(ind, fb, goal, inode);
		}
		fb -= k;
		if (fb < k * k) {
			unsigned dbl = ensureSlotBlock(inode.doubleIndirectPtr, goal, inode);
			if (!dbl)
				return 0;
			unsigned mid = ensureEntry(dbl, fb / k, goal, inode);
			if (!mid)
				return 0;
			return ensureEntry(mid, fb % k, goal, inode);
		}
		fb -= k * k;
		unsigned tri = ensureSlotBlock(inode.tripleIndirectPtr, goal, inode);
		if (!tri)
			return 0;
		unsigned hi = ensureEntry(tri, fb / (k * k), goal, inode);
		if (!hi)
			return 0;
		unsigned mid = ensureEntry(hi, (fb / k) % k, goal, inode);
		if (!mid)
			return 0;
		return ensureEntry(mid, fb % k, goal, inode);
	}

	void truncateBlocks(Ext2Inode& inode, unsigned inodeNo, unsigned firstFree) {
		(void) inodeNo;
		unsigned k = (unsigned) blockSize / 4u;
		for (unsigned i = 0; i < 12; i++)
			if (i >= firstFree && inode.directBlocks[i]) {
				alloc->freeBlock((unsigned) inode.directBlocks[i]);
				inode.directBlocks[i] = 0;
				addBlocksToInode(inode, -1);
			}
		freeTopIndirect(inode.indirectPtr, 1, 12, firstFree, inode);
		freeTopIndirect(inode.doubleIndirectPtr, 2, 12 + k, firstFree, inode);
		freeTopIndirect(inode.tripleIndirectPtr, 3, 12 + k + k * k, firstFree, inode);
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
