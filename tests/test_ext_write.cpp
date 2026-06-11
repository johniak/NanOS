#include "doctest.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "RamBlockDevice.h"
#include <cstdio>
#include <cstring>
// malloc/free come from memory_manager.h (C++ linkage); do not include <cstdlib>.

using namespace kernel;

// Load a filesystem image into a heap buffer and wrap it in a RamBlockDevice. The buffer is
// returned via *bufOut so the test can dump it (after a cache flush) for the e2fsck gate.
static RamBlockDevice* load(const char* path, char** bufOut, long* szOut) {
	FILE* f = fopen(path, "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	char* buf = (char*) malloc((unsigned) sz);
	REQUIRE(fread(buf, 1, (size_t) sz, f) == (size_t) sz);
	fclose(f);
	*bufOut = buf; *szOut = sz;
	return new RamBlockDevice("fixture", buf, (unsigned) sz);
}

static void dump(const char* path, char* buf, long sz) {
	FILE* o = fopen(path, "wb");
	REQUIRE(o != nullptr);
	REQUIRE(fwrite(buf, 1, (size_t) sz, o) == (size_t) sz);
	fclose(o);
}

// Deterministic byte pattern keyed by file offset + a per-file salt.
static unsigned char pat(unsigned off, unsigned salt) { return (unsigned char) ((off * 31u + salt * 7u + 13u) & 0xFF); }

TEST_CASE("ext2 write extends a file through indirect blocks and reads back exactly") {
	char* img; long sz;
	Ext2Filesystem fs(load("tests/fixtures/ext2.img", &img, &sz), 0);
	REQUIRE(fs.mount() == 0);

	// 286720 bytes spans 280 blocks (1 KiB): past the 12 direct pointers, through the whole
	// single-indirect range (12+256 blocks) and into DOUBLE indirect.
	const unsigned N = 280u * 1024u;
	unsigned char* src = (unsigned char*) malloc(N);
	for (unsigned i = 0; i < N; i++) src[i] = pat(i, 1);
	CHECK(fs.write("/hello.txt", N, 0, src) == (int) N);

	FileStat st;
	REQUIRE(fs.stat("/hello.txt", st) == 0);
	CHECK(st.size == (int) N);

	unsigned char* back = (unsigned char*) malloc(N);
	CHECK(fs.read("/hello.txt", N, 0, back) == (int) N);
	for (unsigned i = 0; i < N; i++) REQUIRE(back[i] == src[i]);

	// Shrink back into the direct-block range; freed indirect blocks must leave the fs clean.
	CHECK(fs.truncate("/hello.txt", 3000) == 0);
	REQUIRE(fs.stat("/hello.txt", st) == 0);
	CHECK(st.size == 3000);
	CHECK(fs.read("/hello.txt", 3000, 0, back) == 3000);
	for (unsigned i = 0; i < 3000; i++) REQUIRE(back[i] == src[i]);

	dump("/src/disk/ext2_write.img", img, sz);
	free(src); free(back);
}

TEST_CASE("ext4 fragmented writes grow the extent tree and read back exactly") {
	char* img; long sz;
	Ext4Filesystem fs(load("tests/fixtures/ext4.img", &img, &sz), 0);
	REQUIRE(fs.mount() == 0);

	const unsigned BS = 1024;
	// Two files extended one block at a time, ALTERNATING, so each file's new blocks are
	// physically non-contiguous -> many one-block extents -> the inline 4-entry header overflows
	// and the tree grows to depth >= 1 (exercising extent-block tail checksums under e2fsck).
	FileStat sa, sb;
	REQUIRE(fs.stat("/hello.txt", sa) == 0);
	REQUIRE(fs.stat("/big.txt", sb) == 0);
	unsigned baseA = ((unsigned) sa.size + BS - 1) / BS;   // first free block index in each file
	unsigned baseB = ((unsigned) sb.size + BS - 1) / BS;
	const unsigned ROUNDS = 14;                            // 14 fresh extents/file > inline max 4

	unsigned char blk[BS];
	for (unsigned k = 0; k < ROUNDS; k++) {
		for (unsigned i = 0; i < BS; i++) blk[i] = pat((baseA + k) * BS + i, 2);
		CHECK(fs.write("/hello.txt", BS, (baseA + k) * BS, blk) == (int) BS);
		for (unsigned i = 0; i < BS; i++) blk[i] = pat((baseB + k) * BS + i, 3);
		CHECK(fs.write("/big.txt", BS, (baseB + k) * BS, blk) == (int) BS);
	}

	// Read back every freshly written block of both files.
	unsigned char got[BS];
	for (unsigned k = 0; k < ROUNDS; k++) {
		CHECK(fs.read("/hello.txt", BS, (baseA + k) * BS, got) == (int) BS);
		for (unsigned i = 0; i < BS; i++) REQUIRE(got[i] == pat((baseA + k) * BS + i, 2));
		CHECK(fs.read("/big.txt", BS, (baseB + k) * BS, got) == (int) BS);
		for (unsigned i = 0; i < BS; i++) REQUIRE(got[i] == pat((baseB + k) * BS + i, 3));
	}

	// Truncate hello back to its original size, freeing the grown subtree.
	CHECK(fs.truncate("/hello.txt", (unsigned) sa.size) == 0);
	FileStat s2;
	REQUIRE(fs.stat("/hello.txt", s2) == 0);
	CHECK(s2.size == sa.size);

	dump("/src/disk/ext4_write.img", img, sz);
}

TEST_CASE("ext4 sequential write keeps one extent (len++) and mid-extent truncate splits it") {
	char* img; long sz;
	Ext4Filesystem fs(load("tests/fixtures/ext4.img", &img, &sz), 0);
	REQUIRE(fs.mount() == 0);

	const unsigned BS = 1024;
	FileStat sb;
	REQUIRE(fs.stat("/big.txt", sb) == 0);
	unsigned base = ((unsigned) sb.size + BS - 1) / BS;
	const unsigned BLOCKS = 100;     // a fresh contiguous run -> allocBlock returns sequential
	unsigned char* src = (unsigned char*) malloc(BLOCKS * BS);
	for (unsigned i = 0; i < BLOCKS * BS; i++) src[i] = pat(base * BS + i, 4);
	// One big write so the blocks are allocated back-to-back -> the extent extends via len++.
	CHECK(fs.write("/big.txt", BLOCKS * BS, base * BS, src) == (int) (BLOCKS * BS));

	unsigned char* back = (unsigned char*) malloc(BLOCKS * BS);
	CHECK(fs.read("/big.txt", BLOCKS * BS, base * BS, back) == (int) (BLOCKS * BS));
	for (unsigned i = 0; i < BLOCKS * BS; i++) REQUIRE(back[i] == src[i]);

	// Truncate to halfway through the appended run: cuts inside an extent (the split branch).
	unsigned cut = (base + BLOCKS / 2) * BS;
	CHECK(fs.truncate("/big.txt", cut) == 0);
	FileStat s2;
	REQUIRE(fs.stat("/big.txt", s2) == 0);
	CHECK(s2.size == (int) cut);
	CHECK(fs.read("/big.txt", BS, (base + BLOCKS / 2 - 1) * BS, back) == (int) BS);
	for (unsigned i = 0; i < BS; i++)
		REQUIRE(back[i] == pat((base + BLOCKS / 2 - 1) * BS + i, 4));

	dump("/src/disk/ext4_split.img", img, sz);
	free(src); free(back);
}

TEST_CASE("ext4 deep fragmentation grows the extent tree to depth 2") {
	char* img; long sz;
	Ext4Filesystem fs(load("tests/fixtures/ext4.img", &img, &sz), 0);
	REQUIRE(fs.mount() == 0);

	const unsigned BS = 1024;
	FileStat sa, sbb;
	REQUIRE(fs.stat("/hello.txt", sa) == 0);
	REQUIRE(fs.stat("/big.txt", sbb) == 0);
	unsigned baseA = ((unsigned) sa.size + BS - 1) / BS;
	unsigned baseB = ((unsigned) sbb.size + BS - 1) / BS;
	// A depth-1 inline tree holds 4 index entries; each leaf block holds ~84 extents -> ~336
	// len-1 extents max. Interleaving two files keeps every extent length 1, so > 336 fresh
	// extents in one file forces a second tree level (depth 2).
	const unsigned ROUNDS = 360;
	unsigned char blk[BS];
	for (unsigned k = 0; k < ROUNDS; k++) {
		for (unsigned i = 0; i < BS; i++) blk[i] = pat((baseA + k) * BS + i, 5);
		REQUIRE(fs.write("/hello.txt", BS, (baseA + k) * BS, blk) == (int) BS);
		for (unsigned i = 0; i < BS; i++) blk[i] = pat((baseB + k) * BS + i, 6);
		REQUIRE(fs.write("/big.txt", BS, (baseB + k) * BS, blk) == (int) BS);
	}
	// Spot-check a few blocks across the deep tree.
	unsigned char got[BS];
	for (unsigned k = 0; k < ROUNDS; k += 37) {
		REQUIRE(fs.read("/hello.txt", BS, (baseA + k) * BS, got) == (int) BS);
		for (unsigned i = 0; i < BS; i++) REQUIRE(got[i] == pat((baseA + k) * BS + i, 5));
	}
	// Truncate hello fully back: a depth-2 tree must free cleanly and collapse to inline.
	CHECK(fs.truncate("/hello.txt", (unsigned) sa.size) == 0);
	dump("/src/disk/ext4_deep.img", img, sz);
}

TEST_CASE("ext4 write returns a short count when the disk fills (ENOSPC)") {
	char* img; long sz;
	Ext4Filesystem fs(load("tests/fixtures/ext4.img", &img, &sz), 0);
	REQUIRE(fs.mount() == 0);

	const unsigned BS = 1024;
	FileStat sbb;
	REQUIRE(fs.stat("/big.txt", sbb) == 0);
	unsigned base = ((unsigned) sbb.size + BS - 1) / BS;
	// Ask for far more than the ~2700 free blocks in one write: bmapAlloc eventually gets 0 from
	// the allocator and write() stops early -> a short (but > 0) count.
	const unsigned HUGE = 6000u * BS;
	unsigned char* src = (unsigned char*) malloc(HUGE);
	for (unsigned i = 0; i < HUGE; i++) src[i] = pat(base * BS + i, 7);
	int w = fs.write("/big.txt", HUGE, base * BS, src);
	CHECK(w > 0);
	CHECK(w < (int) HUGE);
	// What was written must read back intact, and the fs must stay consistent.
	unsigned char* back = (unsigned char*) malloc((unsigned) w);
	CHECK(fs.read("/big.txt", (unsigned) w, base * BS, back) == w);
	for (int i = 0; i < w; i++) REQUIRE(back[i] == src[i]);
	dump("/src/disk/ext4_full.img", img, sz);
	free(src); free(back);
}

TEST_CASE("ext write stamps mtime from the wall clock") {
	setBootEpoch(1700000000u);                 // a fixed epoch for the test
	char* img; long sz;
	Ext4Filesystem fs(load("tests/fixtures/ext4.img", &img, &sz), 0);
	REQUIRE(fs.mount() == 0);
	CHECK(fs.write("/hello.txt", 3, 0, "abc") == 3);
	FileStat st;
	REQUIRE(fs.stat("/hello.txt", st) == 0);
	CHECK(st.mtime == 1700000000u);            // wall clock (bootEpoch + 0 ticks in host tests)
	free(img);
	setBootEpoch(0);                           // reset so other tests see currentTime() == 0
}
