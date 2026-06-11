#include "doctest.h"
#include "RamBlockDevice.h"
#include "ext/BlockCache.h"
#include "ext/ExtAllocator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace kernel;

static unsigned rd32(const unsigned char* p, unsigned o) {
	return (unsigned) p[o] | ((unsigned) p[o + 1] << 8) | ((unsigned) p[o + 2] << 16) | ((unsigned) p[o + 3] << 24);
}
static unsigned rd16(const unsigned char* p, unsigned o) { return p[o] | (p[o + 1] << 8); }

// Load a filesystem fixture into a heap buffer the caller owns (used to back a RamBlockDevice).
static unsigned char* loadImg(const char* path, long* outSz) {
	FILE* f = fopen(path, "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char* buf = (unsigned char*) malloc((size_t) sz);
	REQUIRE(fread(buf, 1, (size_t) sz, f) == (size_t) sz);
	fclose(f);
	*outSz = sz;
	return buf;
}

// Read whether a given block-bitmap bit is set, by going through the same descriptor the
// allocator uses (group 0 block bitmap). Lets the test assert at the bitmap level.
static bool blockBitSet(BlockCache& cache, unsigned bitmapBlock, unsigned bit) {
	unsigned char bm[BlockCache::MAX_BLOCK];
	cache.read(bitmapBlock, bm);
	return (bm[bit >> 3] & (1 << (bit & 7))) != 0;
}

// Round-trip alloc/free against a real ext image: a block and an inode get allocated then freed;
// every dependent count must decrement then return to its original value, and the bitmap bit must
// be set then cleared. metadata_csum (ext4 fixture) is recomputed on every metadata write, so the
// dumped image must stay e2fsck-clean (verified separately in the nanos-build container).
static void roundTrip(const char* imgPath, const char* dumpPath) {
	long sz; unsigned char* img = loadImg(imgPath, &sz);
	RamBlockDevice dev("fs", img, (unsigned) sz);
	BlockCache cache(&dev, 0, 1024);

	// The superblock is block 1 (1 KiB blocks); keep a live 1024-byte copy for the allocator.
	unsigned char sb[1024];
	cache.read(1, sb);
	ExtAllocator alloc(&cache, sb);

	unsigned freeBlocks0 = rd32(sb, 0x0C);   // s_free_blocks_count_lo
	unsigned freeInodes0 = rd32(sb, 0x10);   // s_free_inodes_count
	REQUIRE(freeBlocks0 > 0);
	REQUIRE(freeInodes0 > 0);
	bool is64 = (rd32(sb, 0x60) & 0x80) != 0;   // FEATURE_INCOMPAT_64BIT -> used_dirs has a hi half

	// cache.read copies a FULL block (1 KiB), so the descriptor block must be read into a
	// block-sized buffer; group-0 descriptor sits at offset 0. usedDirs() reads its count.
	unsigned char dblk[BlockCache::MAX_BLOCK];
	auto usedDirs = [&]() -> unsigned {
		cache.read(2, dblk);                            // bgdt at block 2 (1 KiB blocks)
		unsigned v = rd16(dblk, 0x10);
		if (is64) v |= rd16(dblk, 0x30) << 16;
		return v;
	};

	// ---- block round trip ----
	unsigned b = alloc.allocBlock(0);
	CHECK(b != 0);
	CHECK(rd32(sb, 0x0C) == freeBlocks0 - 1);

	// Verify the bit is set in group 0's block bitmap (the block came from group 0 with goal 0).
	cache.read(2, dblk);
	unsigned bbBlock = rd32(dblk, 0x00);                    // bg_block_bitmap_lo
	unsigned firstData = rd32(sb, 0x14);
	unsigned bpg = rd32(sb, 0x20);
	unsigned relBit = (b - firstData) % bpg;
	CHECK(blockBitSet(cache, bbBlock, relBit) == true);

	alloc.freeBlock(b);
	CHECK(rd32(sb, 0x0C) == freeBlocks0);
	CHECK(blockBitSet(cache, bbBlock, relBit) == false);

	// ---- inode round trip ----
	unsigned ino = alloc.allocInode(false, 0);
	CHECK(ino != 0);
	CHECK(rd32(sb, 0x10) == freeInodes0 - 1);
	alloc.freeInode(ino, false);
	CHECK(rd32(sb, 0x10) == freeInodes0);

	// ---- directory inode increments used_dirs, free decrements it ----
	unsigned usedDirs0 = usedDirs();
	unsigned dino = alloc.allocInode(true, 0);
	CHECK(dino != 0);
	CHECK(usedDirs() == usedDirs0 + 1);
	alloc.freeInode(dino, true);
	CHECK(usedDirs() == usedDirs0);

	// ---- inode write-back: read root (#2), write it back unchanged ----
	// The bytes are identical and i_checksum is recomputed, so the slot must round-trip exactly
	// (and the dumped image stays e2fsck-clean — exercises writeInode + extInodeCsum end to end).
	unsigned char inoBefore[256], inoAfter[256];
	alloc.readInode(2, inoBefore);
	alloc.writeInode(2, inoBefore);
	alloc.readInode(2, inoAfter);
	for (unsigned i = 0; i < alloc.inodeSize(); i++)
		CHECK(inoAfter[i] == inoBefore[i]);

	cache.flush();

	// Dump the mutated image so the no-shortcuts e2fsck gate can validate it in nanos-build.
	if (dumpPath) {
		FILE* o = fopen(dumpPath, "wb");
		REQUIRE(o != nullptr);
		REQUIRE(fwrite(img, 1, (size_t) sz, o) == (size_t) sz);
		fclose(o);
	}
	free(img);
}

// Dump under /src/disk (the bind mount) so the mutated images persist on the host and the
// no-shortcuts e2fsck gate can validate them in the nanos-build container (which has e2fsprogs).
TEST_CASE("ExtAllocator: block/inode round trip on ext4 (metadata_csum) stays consistent") {
	roundTrip("tests/fixtures/ext4.img", "/src/disk/ext4_alloc.img");
}

TEST_CASE("ExtAllocator: block/inode round trip on ext2 (no csum) stays consistent") {
	roundTrip("tests/fixtures/ext2.img", "/src/disk/ext2_alloc.img");
}

TEST_CASE("ExtAllocator: geometry is derived from the superblock") {
	long sz; unsigned char* img = loadImg("tests/fixtures/ext4.img", &sz);
	RamBlockDevice dev("fs", img, (unsigned) sz);
	BlockCache cache(&dev, 0, 1024);
	unsigned char sb[1024];
	cache.read(1, sb);
	ExtAllocator alloc(&cache, sb);

	CHECK(alloc.blockSize() == 1024u);
	CHECK(alloc.inodeSize() == rd16(sb, 0x58));
	CHECK(alloc.inodesPerGroup() == rd32(sb, 0x28));
	CHECK(alloc.metaCsum() == true);
	CHECK(alloc.groupCount() >= 1u);

	// Root inode (#2) location must match the group-0 inode table.
	unsigned blk, off;
	alloc.inodeLocation(2, blk, off);
	unsigned char dblk[BlockCache::MAX_BLOCK]; cache.read(2, dblk);
	unsigned itable = rd32(dblk, 0x08);
	CHECK(blk == itable + (1u * alloc.inodeSize()) / 1024u);
	CHECK(off == (1u * alloc.inodeSize()) % 1024u);

	free(img);
}
