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
#include "ext/BlockCache.h"
#include "ext/ExtAllocator.h"
#include "ext/ExtCsum.h"
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
	BlockCache* cache;        // all block reads/writes go through here (created in mount())
	ExtAllocator* alloc;      // block/inode allocation + inode read/write (created in mount())
	char superblockBuff[1024];
	char commonBuff[4096];
	char dataBuff[4096];      // data-block read-modify-write scratch (kept off commonBuff)
	Ext2BaseSuperblockFields baseSuperBlock;
	Ext2ExtendedSuperblockFields extendedSuperblock;
	Ext2BlockGroupDescriptor* blockGroupDescriptors;   // allocated to the real group count
	int blockGroupsCount;
	int partitionLba;
	int blockSize;
	int inodeSize;
	int descSize;
public:
	ExtFilesystem(BlockDevice* device, unsigned partitionLba) {
		this->device = device;
		this->partitionLba = partitionLba;
		this->blockGroupDescriptors = 0;
		this->cache = 0;
		this->alloc = 0;
	}

	// Map a file-relative block index to an absolute filesystem block number.
	// ext2 uses direct pointers; ext4 walks an extent tree.
	virtual unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex) = 0;

	// Write seam (format-specific). bmapAlloc returns the block already mapped at
	// fileBlockIndex, or allocates one (plus any indirect/index blocks the mapping needs),
	// records the mapping in `inode`, bumps i_blocks, and returns it (0 = disk full). A freshly
	// allocated DATA block is zero-filled so partial writes never expose stale bytes.
	// truncateBlocks frees every block at fileBlockIndex >= firstFreeBlock (and any now-empty
	// indirect/index blocks), updating i_blocks and clearing the freed pointers in `inode`.
	virtual unsigned bmapAlloc(Ext2Inode& inode, unsigned inodeNo, unsigned fileBlockIndex) = 0;
	virtual void truncateBlocks(Ext2Inode& inode, unsigned inodeNo, unsigned firstFreeBlock) = 0;

	// Initialise a freshly allocated inode's block mapping to "empty": ext2 zeroes the direct/
	// indirect pointers (no extents); ext4 plants an empty extent header in i_block and sets
	// EXTENTS_FL. (Inline fast symlinks bypass this and keep their target in i_block.)
	virtual void initInodeBlockmap(Ext2Inode& inode, bool isDir) = 0;

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
		// All block I/O now flows through the cache (block size is known after initBgdt).
		cache = new BlockCache(device, (unsigned) partitionLba, (unsigned) blockSize);
		// The allocator derives its geometry from the live superblock buffer and writes back
		// through the same cache (so free counts / checksums stay coherent with our reads).
		alloc = new ExtAllocator(cache, (unsigned char*) superblockBuff);
		// printInfo();   // (debug dump) silenced so the boot splash stays one line per step
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
		// The descriptor table grows with the disk and can exceed the 4 KiB scratch buffer
		// (e.g. 64-bit 64-byte descriptors past ~64 groups), so read it into a table-sized
		// buffer and size the descriptor array to the real group count — no fixed [100] cap
		// (which silently overflowed) and no commonBuff overrun on large filesystems.
		blockGroupDescriptors = (Ext2BlockGroupDescriptor*) malloc(
				(unsigned) blockGroupsCount * sizeof(Ext2BlockGroupDescriptor));
		char* bgdtBuf = (char*) malloc((unsigned) sectorCount * 512);
		device->readSectors(this->partitionLba + bgdtBlock * (blockSize / 512),
				sectorCount, bgdtBuf);
		for (int g = 0; g < blockGroupsCount; g++)
			memcpy((void*) &blockGroupDescriptors[g],
					(void*) (bgdtBuf + g * descSize),
					sizeof(Ext2BlockGroupDescriptor));
		free(bgdtBuf);
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
		// Read the whole inode-table block via the cache (the inode never crosses a block
		// boundary, and the full block — not just the first 1 KiB — covers inodes past offset
		// 1024 on 2/4 KiB-block filesystems).
		cache->read((unsigned) blockAddress, commonBuff);
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
			cache->read(block, commonBuff);
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

	bool isSymlink(const Ext2Inode& inode) {
		return ((unsigned short) inode.typeAndPermisions & 0xF000) == 0xA000;
	}

	// Read a symbolic link's target into `buf` (>= 256 bytes), NUL-terminated. ext stores a
	// short target (<= 60 bytes) inline in the 60-byte i_block area ("fast symlink"); a
	// longer one lives in data blocks ("slow symlink"), read via readFile.
	bool readSymlinkTarget(Ext2Inode& inode, char* buf) {
		unsigned len = (unsigned) inode.lowerSize;
		if (len == 0 || len > 255)
			return false;
		if (len <= 60)
			memcpy(buf, &inode.directBlocks[0], len);   // inline target (i_block area)
		else if (readFile(inode, len, 0, buf) < 0)
			return false;
		buf[len] = 0;
		return true;
	}

	// Resolve an absolute path to its inode, following symbolic links. Returns false if any
	// component is missing.
	bool getInodeByPath(String path, Ext2Inode& out) {
		return resolvePath((char*) path, out, 0);
	}

	// Walk an absolute path to its inode by parsing components in place (no String/List
	// allocations per call). A component that resolves to a symlink is followed by resolving
	// its (absolute) target; `depth` guards against symlink loops. Empty components skipped.
	bool resolvePath(const char* p, Ext2Inode& out, int depth) {
		if (depth > 8 || !p || p[0] != '/')
			return false;
		Ext2Inode cur = getInode(2);   // ext2 root inode is #2
		int i = 1;
		while (p[i]) {
			int j = i;
			while (p[j] && p[j] != '/')
				j++;
			int len = j - i;
			if (len > 0) {
				Ext2Inode child;
				if (!getChildrenInode(cur, p + i, len, child))
					return false;
				if (isSymlink(child)) {            // follow the link (absolute target only)
					char tgt[256];
					if (!readSymlinkTarget(child, tgt) || tgt[0] != '/')
						return false;
					if (!resolvePath(tgt, child, depth + 1))
						return false;
				}
				cur = child;
			}
			i = (p[j] == '/') ? j + 1 : j;
		}
		out = cur;
		return true;
	}

	// Find the child named `name` (exactly `len` chars) in directory `inode`.
	bool getChildrenInode(Ext2Inode inode, const char* name, int len, Ext2Inode& out) {
		if (!isDirectory(inode))
			return false;
		List<Ext2DirectoryEntry> entries = getDirectoriesEntries(inode);
		for (int i = 0; i < entries.getCount(); i++) {
			const char* en = entries[i].name;   // NUL-terminated entry name
			int k = 0;
			while (k < len && en[k] && en[k] == name[k])
				k++;
			if (k == len && en[k] == 0) {
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

	void fillFileStat(FileStat& out, Ext2Inode& inode) {
		out.type = isSymlink(inode) ? NODE_SYMLINK
		         : isDirectory(inode) ? NODE_DIR : NODE_FILE;
		out.size = inode.lowerSize;
		out.mode = (unsigned) (unsigned short) inode.typeAndPermisions;
		out.nlink = (unsigned) (unsigned short) inode.hardlinksCount;
		out.uid = (unsigned) (unsigned short) inode.userId;
		out.gid = (unsigned) (unsigned short) inode.groupId;
		out.mtime = (unsigned) inode.lastmodification;
	}

	int stat(String path, FileStat& out) {
		Ext2Inode inode;
		if (!getInodeByPath(path, inode))   // follows symlinks
			return -1;
		fillFileStat(out, inode);
		return 0;
	}

	// lstat: stat the link itself (do not follow a symlink in the final component).
	int lstat(String path, FileStat& out) {
		Ext2Inode inode;
		if (!getInodeNoFollow(path, inode))
			return -1;
		fillFileStat(out, inode);
		return 0;
	}

	// readlink: copy a symbolic link's target (no trailing NUL, like the syscall). Returns
	// the byte count, -EINVAL if the final component is not a symlink, -ENOENT if missing.
	int readlink(String path, char* buf, unsigned size) {
		Ext2Inode inode;
		if (!getInodeNoFollow(path, inode))
			return -2;                      // -ENOENT
		if (!isSymlink(inode))
			return -22;                     // -EINVAL
		char tgt[256];
		if (!readSymlinkTarget(inode, tgt))
			return -22;
		unsigned n = 0;
		while (tgt[n] && n < size) {
			buf[n] = tgt[n];
			n++;
		}
		return (int) n;
	}

	// Resolve a path to its inode WITHOUT following a symlink in the last component (the
	// parent prefix is still followed normally). For lstat/readlink.
	bool getInodeNoFollow(String path, Ext2Inode& out) {
		const char* p = (char*) path;
		if (!p || p[0] != '/')
			return false;
		int end = 0;
		while (p[end]) end++;
		while (end > 0 && p[end - 1] == '/') end--;   // ignore trailing slashes
		int start = end;
		while (start > 0 && p[start - 1] != '/') start--;
		int len = end - start;
		if (len <= 0) { out = getInode(2); return true; }   // path is "/" -> root
		char pp[256];
		int k = 0;
		for (int i = 0; i < start && k < 255; i++) pp[k++] = p[i];   // prefix incl. trailing '/'
		pp[k] = 0;
		Ext2Inode parent;
		if (!resolvePath(pp, parent, 0))               // follow symlinks in the prefix
			return false;
		return getChildrenInode(parent, p + start, len, out);
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
			return 0;   // at/past EOF: 0 bytes (the Unix convention), NOT an error code
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
			if (blockAddress == 0) {
				// Block 0 is never file data (it is the boot/superblock area), so a 0
				// here means a hole (sparse file) or an unresolvable/corrupt mapping.
				// Read it as zeros — never as disk block 0's bytes (which used to leak
				// the superblock into the file silently).
				memset(out + written, 0, chunk);
			} else {
				cache->read(blockAddress, commonBuff);
				memcpy(out + written, commonBuff + within, chunk);
			}
			written += chunk;
		}
		return size;
	}

	// ---- write path (Phase 2) -------------------------------------------------------------
	// i_blocks is in 512-byte sectors; adjust it by `deltaBlocks` filesystem blocks.
	void addBlocksToInode(Ext2Inode& inode, int deltaBlocks) {
		inode.sectorsCount += deltaBlocks * (blockSize / 512);
	}

	// Zero a freshly allocated block in the cache (so a partial write leaves no stale bytes,
	// and new pointer/index blocks read back as all-holes).
	void zeroBlock(unsigned block) {
		memset(dataBuff, 0, (unsigned) blockSize);
		cache->write(block, dataBuff);
	}

	// Persist a modified inode: read its full on-disk bytes, overlay the 128-byte struct (so the
	// large-inode tail — extra_isize / checksum / xattrs — is preserved), recompute i_checksum
	// and write it back.
	void writeInodeStruct(unsigned inodeNo, Ext2Inode& inode) {
		unsigned char buf[256];
		alloc->readInode(inodeNo, buf);
		memcpy(buf, &inode, sizeof(Ext2Inode));
		alloc->writeInode(inodeNo, buf);
	}

	// getChildrenInode but also yields the child's inode number (needed to write a file by path).
	bool getChildrenInodeNum(Ext2Inode inode, const char* name, int len, Ext2Inode& out, int& outNum) {
		if (!isDirectory(inode))
			return false;
		List<Ext2DirectoryEntry> entries = getDirectoriesEntries(inode);
		for (int i = 0; i < entries.getCount(); i++) {
			const char* en = entries[i].name;
			int k = 0;
			while (k < len && en[k] && en[k] == name[k])
				k++;
			if (k == len && en[k] == 0) {
				outNum = entries[i].inode;
				out = getInode(outNum);
				return true;
			}
		}
		return false;
	}

	// resolvePath that also returns the final inode number (follows symlinks like open()).
	bool resolvePathNum(const char* p, Ext2Inode& out, int& outNum, int depth) {
		if (depth > 8 || !p || p[0] != '/')
			return false;
		int curNum = 2;
		Ext2Inode cur = getInode(2);
		int i = 1;
		while (p[i]) {
			int j = i;
			while (p[j] && p[j] != '/')
				j++;
			int len = j - i;
			if (len > 0) {
				Ext2Inode child;
				int childNum;
				if (!getChildrenInodeNum(cur, p + i, len, child, childNum))
					return false;
				if (isSymlink(child)) {
					char tgt[256];
					if (!readSymlinkTarget(child, tgt) || tgt[0] != '/')
						return false;
					if (!resolvePathNum(tgt, child, childNum, depth + 1))
						return false;
				}
				cur = child;
				curNum = childNum;
			}
			i = (p[j] == '/') ? j + 1 : j;
		}
		out = cur;
		outNum = curNum;
		return true;
	}

	// Write [offset, offset+size) of an existing file, allocating blocks as needed. Returns the
	// number of bytes written (a short count if the disk fills), or a negative errno.
	int write(String path, unsigned size, unsigned offset, const void* buff) {
		Ext2Inode inode;
		int inodeNo;
		if (!resolvePathNum((char*) path, inode, inodeNo, 0))
			return -2;                       // -ENOENT
		if (isDirectory(inode))
			return -21;                      // -EISDIR
		const char* src = (const char*) buff;
		unsigned written = 0;
		while (written < size) {
			unsigned filePos = offset + written;
			unsigned blockIndex = filePos / blockSize;
			unsigned within = filePos % blockSize;
			unsigned chunk = blockSize - within;
			if (chunk > size - written)
				chunk = size - written;
			unsigned blk = bmapAlloc(inode, (unsigned) inodeNo, blockIndex);
			if (blk == 0)
				break;                       // out of space -> short write
			cache->read(blk, dataBuff);
			memcpy(dataBuff + within, src + written, chunk);
			cache->write(blk, dataBuff);
			written += chunk;
		}
		unsigned newEnd = offset + written;
		if (newEnd > (unsigned) inode.lowerSize)
			inode.lowerSize = (int) newEnd;
		writeInodeStruct((unsigned) inodeNo, inode);
		cache->flush();
		return (int) written;
	}

	// Shrink or grow a file to `length`. Shrinking frees the blocks past the new end; growing
	// just sets the size (the tail becomes a sparse hole, materialised on the next write).
	int truncate(String path, unsigned length) {
		Ext2Inode inode;
		int inodeNo;
		if (!resolvePathNum((char*) path, inode, inodeNo, 0))
			return -2;
		if (isDirectory(inode))
			return -21;
		unsigned oldSize = (unsigned) inode.lowerSize;
		if (length < oldSize) {
			unsigned firstFreeBlock = (length + blockSize - 1) / blockSize;
			truncateBlocks(inode, (unsigned) inodeNo, firstFreeBlock);
		}
		inode.lowerSize = (int) length;
		writeInodeStruct((unsigned) inodeNo, inode);
		cache->flush();
		return 0;
	}

	// ---- directory operations (Phase 3) ---------------------------------------------------
	enum { FT_REG = 1, FT_DIR = 2, FT_SYMLINK = 7 };

	unsigned round4(unsigned n) { return (n + 3u) & ~3u; }
	// Bytes of a directory block usable for entries (the metadata_csum tail takes the last 12).
	unsigned dirUsable() { return (unsigned) blockSize - (alloc->metaCsum() ? 12u : 0u); }

	// Stamp a directory block's metadata_csum tail (if enabled) then write it back through cache.
	void dirStampAndWrite(unsigned blk, unsigned char* buf, unsigned dirNo, Ext2Inode& dir) {
		if (alloc->metaCsum()) {
			unsigned u = (unsigned) blockSize - 12u;
			buf[u] = buf[u + 1] = buf[u + 2] = buf[u + 3] = 0;   // fake-dirent inode = 0
			buf[u + 4] = 12; buf[u + 5] = 0;                     // rec_len = 12
			buf[u + 6] = 0; buf[u + 7] = 0xDE;                   // name_len=0, fake file_type
			unsigned iseed = extInodeSeed(alloc->csumSeed(), dirNo, (unsigned) dir.generationNumber);
			unsigned c = extDirBlockCsum(iseed, buf, (unsigned) blockSize);
			buf[blockSize - 4] = (unsigned char) c; buf[blockSize - 3] = (unsigned char) (c >> 8);
			buf[blockSize - 2] = (unsigned char) (c >> 16); buf[blockSize - 1] = (unsigned char) (c >> 24);
		}
		cache->write(blk, buf);
	}

	void dirPutEntry(unsigned char* buf, unsigned off, unsigned ino, unsigned recLen,
	                 const char* name, int nameLen, unsigned char ft) {
		buf[off] = (unsigned char) ino; buf[off + 1] = (unsigned char) (ino >> 8);
		buf[off + 2] = (unsigned char) (ino >> 16); buf[off + 3] = (unsigned char) (ino >> 24);
		buf[off + 4] = (unsigned char) recLen; buf[off + 5] = (unsigned char) (recLen >> 8);
		buf[off + 6] = (unsigned char) nameLen; buf[off + 7] = ft;
		for (int i = 0; i < nameLen; i++) buf[off + 8 + i] = (unsigned char) name[i];
	}

	// Insert (name -> ino, type ft) into directory `dir`: reuse a free slot or split an entry's
	// slack, else append a fresh directory block. Grows dir.lowerSize; caller persists the inode.
	bool dirAddEntry(Ext2Inode& dir, unsigned dirNo, const char* name, int nameLen,
	                 unsigned ino, unsigned char ft) {
		unsigned need = 8u + round4((unsigned) nameLen);
		unsigned usable = dirUsable();
		unsigned nblocks = ((unsigned) dir.lowerSize + blockSize - 1) / blockSize;
		unsigned char buf[4096];
		for (unsigned fb = 0; fb < nblocks; fb++) {
			unsigned blk = resolveBlock(dir, fb);
			if (!blk) continue;
			cache->read(blk, buf);
			unsigned off = 0;
			while (off + 8 <= usable) {
				unsigned eino = (unsigned) buf[off] | ((unsigned) buf[off + 1] << 8)
				              | ((unsigned) buf[off + 2] << 16) | ((unsigned) buf[off + 3] << 24);
				unsigned rec = buf[off + 4] | (buf[off + 5] << 8);
				unsigned nl = buf[off + 6];
				if (rec == 0) break;
				if (eino == 0) {
					if (rec >= need) {
						dirPutEntry(buf, off, ino, rec, name, nameLen, ft);
						dirStampAndWrite(blk, buf, dirNo, dir);
						return true;
					}
				} else {
					unsigned actual = 8u + round4(nl);
					if (rec - actual >= need) {
						buf[off + 4] = (unsigned char) actual; buf[off + 5] = (unsigned char) (actual >> 8);
						dirPutEntry(buf, off + actual, ino, rec - actual, name, nameLen, ft);
						dirStampAndWrite(blk, buf, dirNo, dir);
						return true;
					}
				}
				off += rec;
			}
		}
		unsigned fb = nblocks;                          // no room -> append a fresh dir block
		unsigned blk = bmapAlloc(dir, dirNo, fb);
		if (!blk) return false;
		memset(buf, 0, (unsigned) blockSize);
		dirPutEntry(buf, 0, ino, usable, name, nameLen, ft);
		dirStampAndWrite(blk, buf, dirNo, dir);
		dir.lowerSize += blockSize;
		return true;
	}

	// Remove the entry named `name` from `dir` (merge its space into the previous entry, or zero
	// its inode if it is first in the block). Returns true if found.
	bool dirRemoveEntry(Ext2Inode& dir, unsigned dirNo, const char* name, int nameLen) {
		unsigned usable = dirUsable();
		unsigned nblocks = ((unsigned) dir.lowerSize + blockSize - 1) / blockSize;
		unsigned char buf[4096];
		for (unsigned fb = 0; fb < nblocks; fb++) {
			unsigned blk = resolveBlock(dir, fb);
			if (!blk) continue;
			cache->read(blk, buf);
			unsigned off = 0, prev = 0xFFFFFFFFu;
			while (off + 8 <= usable) {
				unsigned eino = (unsigned) buf[off] | ((unsigned) buf[off + 1] << 8)
				              | ((unsigned) buf[off + 2] << 16) | ((unsigned) buf[off + 3] << 24);
				unsigned rec = buf[off + 4] | (buf[off + 5] << 8);
				unsigned nl = buf[off + 6];
				if (rec == 0) break;
				if (eino != 0 && (int) nl == nameLen) {
					bool match = true;
					for (int i = 0; i < nameLen; i++)
						if (buf[off + 8 + i] != (unsigned char) name[i]) { match = false; break; }
					if (match) {
						if (prev != 0xFFFFFFFFu) {
							unsigned prec = buf[prev + 4] | (buf[prev + 5] << 8);
							prec += rec;
							buf[prev + 4] = (unsigned char) prec; buf[prev + 5] = (unsigned char) (prec >> 8);
						} else {
							buf[off] = buf[off + 1] = buf[off + 2] = buf[off + 3] = 0;
						}
						dirStampAndWrite(blk, buf, dirNo, dir);
						return true;
					}
				}
				prev = off;
				off += rec;
			}
		}
		return false;
	}

	// True if `dir` contains only "." and "..".
	bool dirIsEmpty(Ext2Inode& dir) {
		unsigned usable = dirUsable();
		unsigned nblocks = ((unsigned) dir.lowerSize + blockSize - 1) / blockSize;
		unsigned char buf[4096];
		for (unsigned fb = 0; fb < nblocks; fb++) {
			unsigned blk = resolveBlock(dir, fb);
			if (!blk) continue;
			cache->read(blk, buf);
			unsigned off = 0;
			while (off + 8 <= usable) {
				unsigned eino = (unsigned) buf[off] | ((unsigned) buf[off + 1] << 8)
				              | ((unsigned) buf[off + 2] << 16) | ((unsigned) buf[off + 3] << 24);
				unsigned rec = buf[off + 4] | (buf[off + 5] << 8);
				unsigned nl = buf[off + 6];
				if (rec == 0) break;
				if (eino != 0) {
					bool dot = (nl == 1 && buf[off + 8] == '.');
					bool dotdot = (nl == 2 && buf[off + 8] == '.' && buf[off + 9] == '.');
					if (!dot && !dotdot) return false;
				}
				off += rec;
			}
		}
		return true;
	}

	// Write a freshly allocated inode: zero the full inodeSize, overlay the 128-byte struct, set
	// i_extra_isize, recompute i_checksum. (Its on-disk bytes were stale before allocation.)
	void writeNewInode(unsigned inodeNo, Ext2Inode& inode) {
		unsigned char buf[256];
		memset(buf, 0, inodeSize);
		memcpy(buf, &inode, sizeof(Ext2Inode));
		if (inodeSize > 128) { buf[0x80] = 32; buf[0x81] = 0; }   // i_extra_isize = 32
		alloc->writeInode(inodeNo, buf);
	}

	// Resolve `path`'s parent directory and split off the final component into name/nameLen.
	bool resolveParent(const char* path, Ext2Inode& parent, int& parentNo, char* name, int& nameLen) {
		if (!path || path[0] != '/') return false;
		int end = 0; while (path[end]) end++;
		while (end > 0 && path[end - 1] == '/') end--;
		int start = end; while (start > 0 && path[start - 1] != '/') start--;
		nameLen = end - start;
		if (nameLen <= 0 || nameLen > 255) return false;
		for (int i = 0; i < nameLen; i++) name[i] = path[start + i];
		name[nameLen] = 0;
		char pp[256]; int k = 0;
		for (int i = 0; i < start && k < 255; i++) pp[k++] = path[i];
		if (k == 0) pp[k++] = '/';
		pp[k] = 0;
		return resolvePathNum(pp, parent, parentNo, 0);
	}

	unsigned inodeGoal(unsigned dirNo) { return (dirNo - 1) / (unsigned) baseSuperBlock.inodesInGroup; }

	// ---- VFS namespace operations ----
	int create(String path, unsigned mode) {
		Ext2Inode parent; int parentNo; char name[256]; int nl;
		if (!resolveParent((char*) path, parent, parentNo, name, nl)) return -2;
		Ext2Inode existing; int exNo;
		if (getChildrenInodeNum(parent, name, nl, existing, exNo)) {
			if (isDirectory(existing)) return -21;
			return truncate(path, 0);                       // make-or-truncate an existing file
		}
		unsigned ino = alloc->allocInode(false, inodeGoal((unsigned) parentNo));
		if (!ino) return -28;
		Ext2Inode ni; memset(&ni, 0, sizeof(ni));
		ni.typeAndPermisions = (short) (0x8000 | (mode & 0xFFF));
		ni.hardlinksCount = 1;
		initInodeBlockmap(ni, false);
		writeNewInode(ino, ni);
		if (!dirAddEntry(parent, (unsigned) parentNo, name, nl, ino, FT_REG)) {
			alloc->freeInode(ino, false); return -28;
		}
		writeInodeStruct((unsigned) parentNo, parent);
		cache->flush();
		return 0;
	}

	int mkdir(String path, unsigned mode) {
		Ext2Inode parent; int parentNo; char name[256]; int nl;
		if (!resolveParent((char*) path, parent, parentNo, name, nl)) return -2;
		Ext2Inode dummy; int dn;
		if (getChildrenInodeNum(parent, name, nl, dummy, dn)) return -17;   // -EEXIST
		unsigned ino = alloc->allocInode(true, inodeGoal((unsigned) parentNo));
		if (!ino) return -28;
		Ext2Inode ni; memset(&ni, 0, sizeof(ni));
		ni.typeAndPermisions = (short) (0x4000 | (mode & 0xFFF));
		ni.hardlinksCount = 2;
		initInodeBlockmap(ni, true);
		writeNewInode(ino, ni);
		unsigned blk = bmapAlloc(ni, ino, 0);
		if (!blk) { alloc->freeInode(ino, true); return -28; }
		ni.lowerSize = blockSize;
		unsigned char buf[4096]; memset(buf, 0, (unsigned) blockSize);
		dirPutEntry(buf, 0, ino, 12, ".", 1, FT_DIR);
		dirPutEntry(buf, 12, (unsigned) parentNo, dirUsable() - 12, "..", 2, FT_DIR);
		dirStampAndWrite(blk, buf, ino, ni);
		writeInodeStruct(ino, ni);
		if (!dirAddEntry(parent, (unsigned) parentNo, name, nl, ino, FT_DIR)) {
			alloc->freeInode(ino, true); return -28;
		}
		parent.hardlinksCount = (short) (parent.hardlinksCount + 1);   // child's ".." backlink
		writeInodeStruct((unsigned) parentNo, parent);
		cache->flush();
		return 0;
	}

	int unlink(String path) {
		Ext2Inode parent; int parentNo; char name[256]; int nl;
		if (!resolveParent((char*) path, parent, parentNo, name, nl)) return -2;
		Ext2Inode child; int childNo;
		if (!getChildrenInodeNum(parent, name, nl, child, childNo)) return -2;
		if (isDirectory(child)) return -21;                 // use rmdir
		if (!dirRemoveEntry(parent, (unsigned) parentNo, name, nl)) return -2;
		child.hardlinksCount = (short) (child.hardlinksCount - 1);
		if (child.hardlinksCount <= 0) {
			truncateBlocks(child, (unsigned) childNo, 0);
			alloc->freeInode((unsigned) childNo, false);
		} else {
			writeInodeStruct((unsigned) childNo, child);
		}
		cache->flush();
		return 0;
	}

	int rmdir(String path) {
		Ext2Inode parent; int parentNo; char name[256]; int nl;
		if (!resolveParent((char*) path, parent, parentNo, name, nl)) return -2;
		Ext2Inode child; int childNo;
		if (!getChildrenInodeNum(parent, name, nl, child, childNo)) return -2;
		if (!isDirectory(child)) return -20;                // -ENOTDIR
		if (!dirIsEmpty(child)) return -39;                 // -ENOTEMPTY
		if (!dirRemoveEntry(parent, (unsigned) parentNo, name, nl)) return -2;
		truncateBlocks(child, (unsigned) childNo, 0);
		alloc->freeInode((unsigned) childNo, true);
		parent.hardlinksCount = (short) (parent.hardlinksCount - 1);
		writeInodeStruct((unsigned) parentNo, parent);
		cache->flush();
		return 0;
	}

	int link(String oldpath, String newpath) {
		Ext2Inode target; int targetNo;
		if (!resolvePathNum((char*) oldpath, target, targetNo, 0)) return -2;
		if (isDirectory(target)) return -1;                 // -EPERM
		Ext2Inode parent; int parentNo; char name[256]; int nl;
		if (!resolveParent((char*) newpath, parent, parentNo, name, nl)) return -2;
		Ext2Inode dummy; int dn;
		if (getChildrenInodeNum(parent, name, nl, dummy, dn)) return -17;
		unsigned char ft = isSymlink(target) ? FT_SYMLINK : FT_REG;
		if (!dirAddEntry(parent, (unsigned) parentNo, name, nl, (unsigned) targetNo, ft)) return -28;
		writeInodeStruct((unsigned) parentNo, parent);
		target.hardlinksCount = (short) (target.hardlinksCount + 1);
		writeInodeStruct((unsigned) targetNo, target);
		cache->flush();
		return 0;
	}

	int symlink(String target, String path) {
		Ext2Inode parent; int parentNo; char name[256]; int nl;
		if (!resolveParent((char*) path, parent, parentNo, name, nl)) return -2;
		Ext2Inode dummy; int dn;
		if (getChildrenInodeNum(parent, name, nl, dummy, dn)) return -17;
		const char* tgt = (char*) target;
		int tlen = 0; while (tgt[tlen]) tlen++;
		if (tlen > 255) return -22;
		unsigned ino = alloc->allocInode(false, inodeGoal((unsigned) parentNo));
		if (!ino) return -28;
		Ext2Inode ni; memset(&ni, 0, sizeof(ni));
		ni.typeAndPermisions = (short) (0xA000 | 0x1FF);
		ni.hardlinksCount = 1;
		ni.lowerSize = tlen;
		if (tlen <= 60) {                                   // fast symlink: target inline in i_block
			ni.flags &= ~0x80000;
			memcpy(&ni.directBlocks[0], tgt, tlen);
			writeNewInode(ino, ni);
		} else {                                            // slow symlink: a data block
			initInodeBlockmap(ni, false);
			writeNewInode(ino, ni);
			unsigned blk = bmapAlloc(ni, ino, 0);
			if (!blk) { alloc->freeInode(ino, false); return -28; }
			unsigned char buf[4096]; memset(buf, 0, (unsigned) blockSize);
			memcpy(buf, tgt, tlen);
			cache->write(blk, buf);
			writeInodeStruct(ino, ni);
		}
		if (!dirAddEntry(parent, (unsigned) parentNo, name, nl, ino, FT_SYMLINK)) {
			alloc->freeInode(ino, false); return -28;
		}
		writeInodeStruct((unsigned) parentNo, parent);
		cache->flush();
		return 0;
	}

	int rename(String oldpath, String newpath) {
		Ext2Inode oldParent; int oldParentNo; char oldName[256]; int oldNl;
		if (!resolveParent((char*) oldpath, oldParent, oldParentNo, oldName, oldNl)) return -2;
		Ext2Inode src; int srcNo;
		if (!getChildrenInodeNum(oldParent, oldName, oldNl, src, srcNo)) return -2;
		Ext2Inode newParent; int newParentNo; char newName[256]; int newNl;
		if (!resolveParent((char*) newpath, newParent, newParentNo, newName, newNl)) return -2;
		Ext2Inode dst; int dstNo;
		if (getChildrenInodeNum(newParent, newName, newNl, dst, dstNo)) {   // replace destination
			if (isDirectory(dst)) {
				if (!dirIsEmpty(dst)) return -39;
				dirRemoveEntry(newParent, (unsigned) newParentNo, newName, newNl);
				truncateBlocks(dst, (unsigned) dstNo, 0);
				alloc->freeInode((unsigned) dstNo, true);
				newParent.hardlinksCount = (short) (newParent.hardlinksCount - 1);
			} else {
				dirRemoveEntry(newParent, (unsigned) newParentNo, newName, newNl);
				dst.hardlinksCount = (short) (dst.hardlinksCount - 1);
				if (dst.hardlinksCount <= 0) {
					truncateBlocks(dst, (unsigned) dstNo, 0);
					alloc->freeInode((unsigned) dstNo, false);
				} else writeInodeStruct((unsigned) dstNo, dst);
			}
		}
		unsigned char ft = isDirectory(src) ? FT_DIR : isSymlink(src) ? FT_SYMLINK : FT_REG;
		bool sameDir = (oldParentNo == newParentNo);
		if (!dirAddEntry(newParent, (unsigned) newParentNo, newName, newNl, (unsigned) srcNo, ft))
			return -28;
		if (sameDir) {
			// Re-read so removing the old name sees the just-added entry (same block buffer).
			writeInodeStruct((unsigned) newParentNo, newParent);
			newParent = getInode(newParentNo);
			dirRemoveEntry(newParent, (unsigned) newParentNo, oldName, oldNl);
			writeInodeStruct((unsigned) newParentNo, newParent);
		} else {
			dirRemoveEntry(oldParent, (unsigned) oldParentNo, oldName, oldNl);
			if (isDirectory(src)) {
				updateDotDot(src, (unsigned) srcNo, (unsigned) newParentNo);
				oldParent.hardlinksCount = (short) (oldParent.hardlinksCount - 1);
				newParent.hardlinksCount = (short) (newParent.hardlinksCount + 1);
			}
			writeInodeStruct((unsigned) oldParentNo, oldParent);
			writeInodeStruct((unsigned) newParentNo, newParent);
		}
		cache->flush();
		return 0;
	}

	// Repoint a directory's ".." entry to a new parent inode (used by cross-directory rename).
	void updateDotDot(Ext2Inode& dir, unsigned dirNo, unsigned newParentNo) {
		unsigned blk = resolveBlock(dir, 0);
		if (!blk) return;
		unsigned char buf[4096];
		cache->read(blk, buf);
		unsigned usable = dirUsable(), off = 0;
		while (off + 8 <= usable) {
			unsigned rec = buf[off + 4] | (buf[off + 5] << 8);
			unsigned nl = buf[off + 6];
			if (rec == 0) break;
			if (nl == 2 && buf[off + 8] == '.' && buf[off + 9] == '.') {
				buf[off] = (unsigned char) newParentNo; buf[off + 1] = (unsigned char) (newParentNo >> 8);
				buf[off + 2] = (unsigned char) (newParentNo >> 16); buf[off + 3] = (unsigned char) (newParentNo >> 24);
				dirStampAndWrite(blk, buf, dirNo, dir);
				return;
			}
			off += rec;
		}
	}

	// ---- metadata mutations + stats (Phase 4) ---------------------------------------------
	int chmod(String path, unsigned mode) {
		Ext2Inode inode; int no;
		if (!resolvePathNum((char*) path, inode, no, 0)) return -2;
		inode.typeAndPermisions = (short) ((inode.typeAndPermisions & 0xF000) | (mode & 0xFFF));
		writeInodeStruct((unsigned) no, inode);
		cache->flush();
		return 0;
	}

	int chown(String path, unsigned uid, unsigned gid) {
		Ext2Inode inode; int no;
		if (!resolvePathNum((char*) path, inode, no, 0)) return -2;
		if (uid != 0xFFFFFFFFu) inode.userId = (short) uid;     // -1 leaves the field unchanged
		if (gid != 0xFFFFFFFFu) inode.groupId = (short) gid;
		writeInodeStruct((unsigned) no, inode);
		cache->flush();
		return 0;
	}

	int utimes(String path, unsigned atime, unsigned mtime) {
		Ext2Inode inode; int no;
		if (!resolvePathNum((char*) path, inode, no, 0)) return -2;
		inode.lastAccess = (int) atime;
		inode.lastmodification = (int) mtime;
		writeInodeStruct((unsigned) no, inode);
		cache->flush();
		return 0;
	}

	int statfs(String path, StatFs& out) {
		(void) path;
		out.blockSize = (unsigned) blockSize;
		out.totalBlocks = (unsigned) baseSuperBlock.totalBlocks;
		out.freeBlocks = *(unsigned*) (superblockBuff + 0x0C);   // live free counts (allocator-kept)
		out.totalInodes = (unsigned) baseSuperBlock.totalInodes;
		out.freeInodes = *(unsigned*) (superblockBuff + 0x10);
		out.nameMax = 255;
		return 0;
	}
};

} /* namespace kernel */

#endif /* EXTFILESYSTEM_H_ */
