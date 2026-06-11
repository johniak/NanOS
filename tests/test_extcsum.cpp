#include "doctest.h"
#include "ext/ExtCsum.h"
#include <cstdio>
#include <cstdlib>

using namespace kernel;

static unsigned short rd16(const unsigned char* p, unsigned o) { return p[o] | (p[o + 1] << 8); }
static unsigned       rd32(const unsigned char* p, unsigned o) {
	return (unsigned) p[o] | ((unsigned) p[o + 1] << 8) | ((unsigned) p[o + 2] << 16) | ((unsigned) p[o + 3] << 24);
}

// Load the committed ext4 fixture (metadata_csum + crc32c) so we can check our checksum math
// against what e2fsprogs actually wrote — the gold standard for "no shortcuts".
static unsigned char* loadExt4(long* outSz) {
	FILE* f = fopen("tests/fixtures/ext4.img", "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char* buf = (unsigned char*) malloc((size_t) sz);
	REQUIRE(fread(buf, 1, (size_t) sz, f) == (size_t) sz);
	fclose(f);
	*outSz = sz;
	return buf;
}

TEST_CASE("ExtCsum: superblock + group-descriptor checksums match the real ext4 image") {
	long sz; unsigned char* img = loadExt4(&sz);
	const unsigned char* sb = img + 1024;            // superblock at byte 1024

	REQUIRE(extHasMetadataCsum(sb));                 // fixture has metadata_csum

	// Superblock self-checksum: crc32c(~0, sb, 0x3FC) must equal stored s_checksum.
	CHECK(extSuperblockCsum(sb) == rd32(sb, 0x3FC));

	// Group descriptor 0: 1 KiB blocks -> bgdt starts at block 2 (byte 2048); s_desc_size @ 0xFE.
	unsigned blockSize = 1024u << rd32(sb, 0x18);    // s_log_block_size
	CHECK(blockSize == 1024u);
	unsigned descSize = rd16(sb, 0xFE);
	CHECK(descSize >= 32u);
	unsigned seed = extCsumSeed(sb);

	const unsigned char* desc0 = img + 2 * blockSize;
	unsigned short stored = rd16(desc0, EXT_GD_CHECKSUM_OFFSET);
	unsigned short computed = extGroupDescCsum(seed, 0, desc0, descSize);
	CHECK(computed == stored);

	free(img);
}

TEST_CASE("ExtCsum: inode checksum round-trips (recompute matches the stored value)") {
	long sz; unsigned char* img = loadExt4(&sz);
	const unsigned char* sb = img + 1024;
	unsigned seed = extCsumSeed(sb);
	unsigned inodeSize = rd16(sb, 0x58);             // s_inode_size
	CHECK(inodeSize == 256u);

	// Root inode (#2): inode table block from group desc 0 (bg_inode_table_lo @ 0x08),
	// inode index 1 (#2-1) within it. 1 KiB blocks.
	unsigned blockSize = 1024u;
	const unsigned char* desc0 = img + 2 * blockSize;
	unsigned itable = rd32(desc0, 0x08);
	unsigned off = itable * blockSize + 1u * inodeSize;   // inode #2
	unsigned char ino[256];
	for (unsigned i = 0; i < inodeSize; i++) ino[i] = img[off + i];

	unsigned short stored = ino[0x7C] | (ino[0x7D] << 8);
	extInodeCsum(seed, 2, ino, inodeSize);                // recompute into ino[]
	unsigned short recomputed = ino[0x7C] | (ino[0x7D] << 8);
	CHECK(recomputed == stored);

	free(img);
}
