#include "doctest.h"
#include "Ext4Filesystem.h"
#include "Ext2Filesystem.h"
#include "RamBlockDevice.h"
#include "Vfs.h"
#include <cstdio>
#include <cstring>
// malloc/free come from memory_manager.h (transitive); do not include <cstdlib>.

using namespace kernel;

static RamBlockDevice* loadExt4() {
	FILE* f = fopen("tests/fixtures/ext4.img", "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = (char*) malloc(sz);
	REQUIRE(fread(buf, 1, sz, f) == (size_t) sz);
	fclose(f);
	return new RamBlockDevice("ext4", buf, (unsigned) sz);
}

TEST_CASE("ext4 mounts and reads a small file via extents") {
	Ext4Filesystem fs(loadExt4(), 0);
	REQUIRE(fs.mount() == 0);
	char buf[64] = {0};
	int n = fs.read("/hello.txt", sizeof(buf) - 1, 0, buf);
	REQUIRE(n > 0);
	buf[n] = 0;
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);
}

TEST_CASE("ext4 readdir lists directory entries") {
	Ext4Filesystem fs(loadExt4(), 0);
	fs.mount();
	List<DirEntry> e;
	REQUIRE(fs.readdir("/boot/grub", e) == 0);
	bool found = false;
	for (int i = 0; i < e.getCount(); i++)
		if (strcmp(e[i].name, "grub.cfg") == 0)
			found = true;
	CHECK(found);
}

TEST_CASE("ext4 reads a multi-block file fully (extent spanning)") {
	Ext4Filesystem fs(loadExt4(), 0);
	fs.mount();
	FileStat st;
	REQUIRE(fs.stat("/big.txt", st) == 0);
	CHECK(st.size == 5000);
	char* buf = (char*) malloc(st.size + 1);
	int n = fs.read("/big.txt", st.size, 0, buf);
	CHECK(n == (int) st.size);
	CHECK(buf[0] == 'N');          // "NANOS-EXT4-..."
	CHECK(buf[4999] != 0);         // last byte present (no hole)
}

TEST_CASE("ext4 resolveBlock falls back to direct blocks without EXTENTS_FL") {
	Ext4Filesystem fs(loadExt4(), 0);
	fs.mount();
	Ext2Inode inode;
	inode.flags = 0;                 // no extents -> classic direct blocks
	inode.directBlocks[3] = 42;
	CHECK(fs.resolveBlock(inode, 3) == 42);
}

TEST_CASE("ext4 resolveBlock walks a depth-1 extent tree") {
	RamBlockDevice* dev = loadExt4();
	Ext4Filesystem fs(dev, 0);
	fs.mount();                       // blockSize = 1024
	// Lay a leaf node (depth 0) at block 1000 of the device: file block 0 -> 777.
	char leaf[1024];
	memset(leaf, 0, sizeof(leaf));
	Ext4ExtentHeader* lh = (Ext4ExtentHeader*) leaf;
	lh->magic = 0xF30A; lh->entries = 1; lh->max = 4; lh->depth = 0;
	Ext4Extent* lex = (Ext4Extent*) (leaf + sizeof(Ext4ExtentHeader));
	lex->fileBlock = 0; lex->len = 1; lex->startHi = 0; lex->startLo = 777;
	dev->writeSectors(1000 * 2, 2, leaf);   // block 1000 -> LBA 2000

	// Craft an inode whose i_block is a depth-1 header + one index -> block 1000.
	Ext2Inode inode;
	inode.flags = 0x80000;
	char* ib = (char*) &inode.directBlocks[0];
	Ext4ExtentHeader* h = (Ext4ExtentHeader*) ib;
	h->magic = 0xF30A; h->entries = 1; h->max = 4; h->depth = 1;
	Ext4ExtentIdx* idx = (Ext4ExtentIdx*) (ib + sizeof(Ext4ExtentHeader));
	idx->fileBlock = 0; idx->leafLo = 1000; idx->leafHi = 0;

	CHECK(fs.resolveBlock(inode, 0) == 777);
}

TEST_CASE("ext4 resolveBlock returns 0 for a bad extent header") {
	Ext4Filesystem fs(loadExt4(), 0);
	fs.mount();
	Ext2Inode inode;
	inode.flags = 0x80000;
	((Ext4ExtentHeader*) &inode.directBlocks[0])->magic = 0x0000;   // not 0xF30A
	CHECK(fs.resolveBlock(inode, 0) == 0);
}

TEST_CASE("ext4 type probes true on ext4, ext2 probes false") {
	Ext4FileSystemType e4;
	Ext2FileSystemType e2;
	RamBlockDevice* dev = loadExt4();
	CHECK(e4.probe(dev, 0) == true);
	CHECK(e2.probe(dev, 0) == false);
}

TEST_CASE("VFS auto-mount resolves an ext4 image to the ext4 driver") {
	Vfs vfs;
	static Ext2FileSystemType e2;
	static Ext4FileSystemType e4;
	vfs.registerType(&e4);
	vfs.registerType(&e2);
	REQUIRE(vfs.mount("/", "auto", loadExt4(), 0) == 0);
	char buf[64] = {0};
	int n = vfs.read("/hello.txt", sizeof(buf) - 1, 0, buf);
	REQUIRE(n > 0);
	buf[n] = 0;
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);
}
