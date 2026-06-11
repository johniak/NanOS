/*
 * Ext4Filesystem.h
 *
 * ext4 specialisation of the shared ExtFilesystem core. Differs from ext2 only
 * in block mapping: ext4 inodes carry an extent tree in the i_block area
 * instead of direct/indirect pointers. The 64-bit descriptor size is handled by
 * the core (ExtFilesystem::mount reads s_desc_size).
 */
#include "ExtFilesystem.h"
#include "ext/ExtCsum.h"
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
			cache->read(child, extentBuf);
			cur = extentBuf;
		}
	}

	// ---- write seam: extent-tree append (Phase 2) ---------------------------------------
	enum { INS_OK = 0, INS_FULL = 1, INS_ERR = 2, EXT_MAGIC = 0xF30A };

	unsigned goalGroup(unsigned inodeNo) {
		return (inodeNo - 1) / (unsigned) baseSuperBlock.inodesInGroup;
	}
	// Per-inode seed for extent-block tail checksums (0 if metadata_csum is off).
	unsigned inodeSeed(unsigned inodeNo, Ext2Inode& inode) {
		if (!alloc->metaCsum())
			return 0;
		return extInodeSeed(alloc->csumSeed(), inodeNo, (unsigned) inode.generationNumber);
	}
	// Entries an on-disk extent block holds: a full block minus header (12) minus the 4-byte
	// et_checksum tail when metadata_csum is on (so the tail fits).
	unsigned blockNodeMax() {
		unsigned avail = (unsigned) blockSize - 12u - (alloc->metaCsum() ? 4u : 0u);
		return avail / 12u;
	}
	// Write an extent BLOCK back, stamping its tail checksum first (inline trees skip this).
	void writeExtentBlock(unsigned blk, char* buf, unsigned iseed) {
		if (alloc->metaCsum()) {
			Ext4ExtentHeader* h = (Ext4ExtentHeader*) buf;
			unsigned tailOff = 12u + (unsigned) h->max * 12u;
			unsigned c = extExtentBlockCsum(iseed, buf, tailOff);
			*(unsigned*) (buf + tailOff) = c;
		}
		cache->write(blk, buf);
	}

	// Build a fresh subtree of the given depth holding the single mapping fb->nb; return its
	// block (0 = disk full). depth 0 = a leaf with one extent; depth>0 = an index over a child.
	unsigned extentMakeChild(Ext2Inode& inode, unsigned inodeNo, int depth, unsigned fb, unsigned nb) {
		unsigned blk = alloc->allocBlock(goalGroup(inodeNo));
		if (!blk)
			return 0;
		char buf[4096];
		memset(buf, 0, (unsigned) blockSize);
		Ext4ExtentHeader* h = (Ext4ExtentHeader*) buf;
		h->magic = EXT_MAGIC;
		h->entries = 0;
		h->depth = (unsigned short) depth;
		h->generation = 0;
		h->max = (unsigned short) blockNodeMax();
		if (depth == 0) {
			Ext4Extent* ex = (Ext4Extent*) (buf + 12);
			ex[0].fileBlock = fb; ex[0].len = 1; ex[0].startHi = 0; ex[0].startLo = nb;
			h->entries = 1;
		} else {
			unsigned child = extentMakeChild(inode, inodeNo, depth - 1, fb, nb);
			if (!child) { alloc->freeBlock(blk); return 0; }
			Ext4ExtentIdx* ix = (Ext4ExtentIdx*) (buf + 12);
			ix[0].fileBlock = fb; ix[0].leafLo = child; ix[0].leafHi = 0; ix[0].unused = 0;
			h->entries = 1;
		}
		writeExtentBlock(blk, buf, inodeSeed(inodeNo, inode));
		addBlocksToInode(inode, 1);
		return blk;
	}

	// Append mapping fb->nb at the tail of the (sub)tree rooted at `node`. nodeBlock 0 means the
	// inline root in the inode (persisted by the caller); otherwise the node is written back.
	int extentInsertTail(Ext2Inode& inode, unsigned inodeNo, char* node, unsigned nodeBlock,
	                     unsigned fb, unsigned nb) {
		Ext4ExtentHeader* h = (Ext4ExtentHeader*) node;
		if (h->magic != EXT_MAGIC)
			return INS_ERR;
		unsigned iseed = inodeSeed(inodeNo, inode);
		if (h->depth == 0) {
			Ext4Extent* ex = (Ext4Extent*) (node + 12);
			if (h->entries > 0) {
				Ext4Extent& last = ex[h->entries - 1];
				if (last.fileBlock + last.len == fb && last.startLo + last.len == nb
				        && last.len < 32768) {
					last.len = (unsigned short) (last.len + 1);
					if (nodeBlock) writeExtentBlock(nodeBlock, node, iseed);
					return INS_OK;
				}
			}
			if (h->entries < h->max) {
				Ext4Extent& e = ex[h->entries];
				e.fileBlock = fb; e.len = 1; e.startHi = 0; e.startLo = nb;
				h->entries = (unsigned short) (h->entries + 1);
				if (nodeBlock) writeExtentBlock(nodeBlock, node, iseed);
				return INS_OK;
			}
			return INS_FULL;
		}
		Ext4ExtentIdx* ix = (Ext4ExtentIdx*) (node + 12);
		if (h->entries == 0) {
			unsigned child = extentMakeChild(inode, inodeNo, h->depth - 1, fb, nb);
			if (!child) return INS_ERR;
			ix[0].fileBlock = fb; ix[0].leafLo = child; ix[0].leafHi = 0; ix[0].unused = 0;
			h->entries = 1;
			if (nodeBlock) writeExtentBlock(nodeBlock, node, iseed);
			return INS_OK;
		}
		unsigned childBlk = ix[h->entries - 1].leafLo;
		char buf[4096];
		cache->read(childBlk, buf);
		int r = extentInsertTail(inode, inodeNo, buf, childBlk, fb, nb);
		if (r == INS_OK) return INS_OK;
		if (r == INS_ERR) return INS_ERR;
		// child full -> attach a new child subtree to this index node
		if (h->entries < h->max) {
			unsigned newchild = extentMakeChild(inode, inodeNo, h->depth - 1, fb, nb);
			if (!newchild) return INS_ERR;
			ix[h->entries].fileBlock = fb; ix[h->entries].leafLo = newchild;
			ix[h->entries].leafHi = 0; ix[h->entries].unused = 0;
			h->entries = (unsigned short) (h->entries + 1);
			if (nodeBlock) writeExtentBlock(nodeBlock, node, iseed);
			return INS_OK;
		}
		return INS_FULL;
	}

	// Grow the tree one level: move the inline node's contents into a new block and turn the
	// inline root into a 1-entry index pointing at it (so it has room to grow again).
	bool extentGrow(Ext2Inode& inode, unsigned inodeNo) {
		char* root = (char*) &inode.directBlocks[0];
		Ext4ExtentHeader* rh = (Ext4ExtentHeader*) root;
		unsigned newblk = alloc->allocBlock(goalGroup(inodeNo));
		if (!newblk)
			return false;
		char buf[4096];
		memset(buf, 0, (unsigned) blockSize);
		memcpy(buf, root, 12u + (unsigned) rh->entries * 12u);   // header + all inline entries
		Ext4ExtentHeader* bh = (Ext4ExtentHeader*) buf;
		bh->max = (unsigned short) blockNodeMax();
		writeExtentBlock(newblk, buf, inodeSeed(inodeNo, inode));
		addBlocksToInode(inode, 1);
		unsigned firstFb = (rh->depth == 0) ? ((Ext4Extent*) (root + 12))[0].fileBlock
		                                    : ((Ext4ExtentIdx*) (root + 12))[0].fileBlock;
		rh->depth = (unsigned short) (rh->depth + 1);
		rh->entries = 1;
		rh->max = 4;                                             // the inline i_block holds 4
		Ext4ExtentIdx* ix = (Ext4ExtentIdx*) (root + 12);
		ix[0].fileBlock = firstFb; ix[0].leafLo = newblk; ix[0].leafHi = 0; ix[0].unused = 0;
		return true;
	}

	unsigned bmapAlloc(Ext2Inode& inode, unsigned inodeNo, unsigned fb) {
		// Non-extent ext4 inode (rare): classic direct blocks only.
		if (!(inode.flags & 0x80000)) {
			if (fb < 12) {
				if (inode.directBlocks[fb])
					return (unsigned) inode.directBlocks[fb];
				unsigned nb = alloc->allocBlock(goalGroup(inodeNo));
				if (!nb) return 0;
				zeroBlock(nb);
				inode.directBlocks[fb] = (int) nb;
				addBlocksToInode(inode, 1);
				return nb;
			}
			return 0;
		}
		unsigned existing = resolveBlock(inode, fb);
		if (existing)
			return existing;
		unsigned nb = alloc->allocBlock(goalGroup(inodeNo));
		if (!nb)
			return 0;
		zeroBlock(nb);
		char* root = (char*) &inode.directBlocks[0];
		int r = extentInsertTail(inode, inodeNo, root, 0, fb, nb);
		if (r == INS_FULL) {
			if (!extentGrow(inode, inodeNo)) { alloc->freeBlock(nb); return 0; }
			r = extentInsertTail(inode, inodeNo, root, 0, fb, nb);
		}
		if (r != INS_OK) { alloc->freeBlock(nb); return 0; }
		addBlocksToInode(inode, 1);   // count the data block (tree blocks counted in make/grow)
		return nb;
	}

	// Gather every leaf extent of the subtree into out[] (cap entries). Returns the count, or
	// -1 if there are more than `cap` (so the tree cannot collapse into the inline header).
	int gatherExtents(char* node, Ext4Extent* out, int cap, int cnt) {
		Ext4ExtentHeader* h = (Ext4ExtentHeader*) node;
		if (h->magic != EXT_MAGIC)
			return -1;
		if (h->depth == 0) {
			Ext4Extent* ex = (Ext4Extent*) (node + 12);
			for (int i = 0; i < h->entries; i++) {
				if (cnt >= cap)
					return -1;
				out[cnt++] = ex[i];
			}
			return cnt;
		}
		Ext4ExtentIdx* ix = (Ext4ExtentIdx*) (node + 12);
		for (int i = 0; i < h->entries; i++) {
			char buf[4096];
			cache->read(ix[i].leafLo, buf);
			cnt = gatherExtents(buf, out, cap, cnt);
			if (cnt < 0)
				return -1;
		}
		return cnt;
	}

	// Free every index/leaf BLOCK below `node` (post-order); data blocks are left alone.
	void freeTreeBlocks(Ext2Inode& inode, char* node) {
		Ext4ExtentHeader* h = (Ext4ExtentHeader*) node;
		if (h->magic != EXT_MAGIC || h->depth == 0)
			return;
		Ext4ExtentIdx* ix = (Ext4ExtentIdx*) (node + 12);
		for (int i = 0; i < h->entries; i++) {
			char buf[4096];
			cache->read(ix[i].leafLo, buf);
			freeTreeBlocks(inode, buf);
			alloc->freeBlock(ix[i].leafLo);
			addBlocksToInode(inode, -1);
		}
	}

	// If the tree has grown to depth>0 but now holds few enough extents to fit the inline header,
	// collapse it back (freeing the tree blocks). Keeps e2fsck from reporting "could be shorter".
	void maybeCollapseInline(Ext2Inode& inode) {
		char* root = (char*) &inode.directBlocks[0];
		Ext4ExtentHeader* rh = (Ext4ExtentHeader*) root;
		if (rh->magic != EXT_MAGIC || rh->depth == 0)
			return;
		Ext4Extent tmp[4];
		int n = gatherExtents(root, tmp, 4, 0);
		if (n < 0)
			return;                       // too many extents — a tree is genuinely needed
		freeTreeBlocks(inode, root);
		rh->depth = 0;
		rh->max = 4;
		rh->entries = (unsigned short) n;
		Ext4Extent* ex = (Ext4Extent*) (root + 12);
		for (int i = 0; i < n; i++)
			ex[i] = tmp[i];
	}

	// Free all data at file-block index >= firstFree and prune emptied subtrees. Returns true if
	// `node` ends up with no entries (caller frees the node block / resets the inline root).
	bool extentTruncNode(Ext2Inode& inode, unsigned inodeNo, char* node, unsigned nodeBlock,
	                     unsigned firstFree) {
		Ext4ExtentHeader* h = (Ext4ExtentHeader*) node;
		if (h->magic != EXT_MAGIC)
			return false;
		unsigned iseed = inodeSeed(inodeNo, inode);
		bool dirty = false;
		if (h->depth == 0) {
			Ext4Extent* ex = (Ext4Extent*) (node + 12);
			int kept = 0;
			for (int i = 0; i < h->entries; i++) {
				unsigned start = ex[i].fileBlock, len = ex[i].len;
				unsigned end = start + len;
				if (end <= firstFree) { ex[kept++] = ex[i]; continue; }     // wholly kept
				if (start >= firstFree) {                                    // wholly freed
					for (unsigned b = 0; b < len; b++) alloc->freeBlock(ex[i].startLo + b);
					addBlocksToInode(inode, -(int) len);
					dirty = true;
				} else {                                                     // split: keep head
					unsigned keep = firstFree - start;
					for (unsigned b = keep; b < len; b++) alloc->freeBlock(ex[i].startLo + b);
					addBlocksToInode(inode, -(int) (len - keep));
					ex[i].len = (unsigned short) keep;
					ex[kept++] = ex[i];
					dirty = true;
				}
			}
			if (dirty) {
				h->entries = (unsigned short) kept;
				if (nodeBlock) writeExtentBlock(nodeBlock, node, iseed);
			}
			return h->entries == 0;
		}
		Ext4ExtentIdx* ix = (Ext4ExtentIdx*) (node + 12);
		int kept = 0;
		for (int i = 0; i < h->entries; i++) {
			// A child covers [ix[i].fileBlock, nextStart). Cheap test: descend unless the whole
			// child is below the cut (its start >= firstFree => surely all freed).
			unsigned childStart = ix[i].fileBlock;
			unsigned nextStart = (i + 1 < h->entries) ? ix[i + 1].fileBlock : 0xFFFFFFFFu;
			if (nextStart <= firstFree) { ix[kept++] = ix[i]; continue; }   // wholly kept
			char buf[4096];
			unsigned cb = ix[i].leafLo;
			cache->read(cb, buf);
			bool emptied = extentTruncNode(inode, inodeNo, buf, cb, firstFree);
			if (emptied) {
				alloc->freeBlock(cb);
				addBlocksToInode(inode, -1);
				dirty = true;
				(void) childStart;
			} else {
				ix[kept++] = ix[i];
			}
		}
		if (dirty) {
			h->entries = (unsigned short) kept;
			if (nodeBlock) writeExtentBlock(nodeBlock, node, iseed);
		}
		return h->entries == 0;
	}

	void truncateBlocks(Ext2Inode& inode, unsigned inodeNo, unsigned firstFree) {
		if (!(inode.flags & 0x80000)) {           // non-extent fallback: free direct tail
			for (unsigned i = firstFree; i < 12; i++)
				if (inode.directBlocks[i]) {
					alloc->freeBlock((unsigned) inode.directBlocks[i]);
					inode.directBlocks[i] = 0;
					addBlocksToInode(inode, -1);
				}
			return;
		}
		char* root = (char*) &inode.directBlocks[0];
		extentTruncNode(inode, inodeNo, root, 0, firstFree);
		// Collapse a now-oversized tree back into the inline header when the survivors fit, so the
		// inode stays a minimal, e2fsck-optimal extent tree.
		maybeCollapseInline(inode);
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
