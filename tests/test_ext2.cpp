#include "doctest.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "RamBlockDevice.h"
#include "Vfs.h"
#include <cstdio>
#include <cstring>
// malloc/free come from memory_manager.h (included transitively) with C++
// linkage; do not include <cstdlib> (its C-linkage decls would conflict).

using namespace kernel;

// Loads the committed raw-ext2 fixture into RAM. partitionLba = 0 because the
// fixture is a bare filesystem (superblock at byte 1024 = LBA 2 within it).
static RamBlockDevice* loadFixture() {
	FILE* f = fopen("tests/fixtures/ext2.img", "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = (char*) malloc(sz);
	REQUIRE(fread(buf, 1, sz, f) == (size_t) sz);
	fclose(f);
	return new RamBlockDevice("fixture", buf, (unsigned) sz);
}

TEST_CASE("ext2 mounts the fixture filesystem") {
	Ext2Filesystem fs(loadFixture(), 0);
	CHECK(fs.mount() == 0);
}

TEST_CASE("ext2 readdir lists a directory's entries") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	List<DirEntry> entries;
	REQUIRE(fs.readdir("/boot/grub", entries) == 0);
	bool found = false;
	for (int i = 0; i < entries.getCount(); i++)
		if (strcmp(entries[i].name, "grub.cfg") == 0)
			found = true;
	CHECK(found);
}

TEST_CASE("ext2 reads exact file contents") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	char buf[64] = {0};
	int n = fs.read("/hello.txt", sizeof(buf) - 1, 0, buf);
	REQUIRE(n > 0);
	buf[n] = 0;
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);
}

TEST_CASE("ext2 stat reports type and size for files and directories") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	FileStat st;
	REQUIRE(fs.stat("/hello.txt", st) == 0);
	CHECK(st.type == NODE_FILE);
	CHECK(st.size == 18);                 // strlen("Hello, NanOS VFS!\n")
	FileStat dst;
	REQUIRE(fs.stat("/boot", dst) == 0);
	CHECK(dst.type == NODE_DIR);
}

TEST_CASE("ext2 stat surfaces inode metadata (mode/nlink) from the inode") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	FileStat st;
	REQUIRE(fs.stat("/hello.txt", st) == 0);
	CHECK((st.mode & 0xF000) == 0x8000);   // S_IFREG
	CHECK((st.mode & 0x1FF) != 0);         // some permission bits set
	CHECK(st.nlink >= 1);                  // at least one hard link

	FileStat dst;
	REQUIRE(fs.stat("/boot", dst) == 0);
	CHECK((dst.mode & 0xF000) == 0x4000);  // S_IFDIR
	CHECK(dst.nlink >= 2);                 // a dir links itself (.) and its parent
}

TEST_CASE("ext2 reports errors for missing paths instead of crashing") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	char buf[8];
	CHECK(fs.read("/nope", 1, 0, buf) < 0);
	FileStat st;
	CHECK(fs.stat("/no/such/path", st) < 0);
	CHECK(fs.read("notabsolute", 1, 0, buf) < 0);
}

TEST_CASE("ext2 reads honour offset and clamp to file size") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	char buf[64] = {0};
	// "Hello, NanOS VFS!\n" — read 5 bytes from offset 7 -> "NanOS"
	int n = fs.read("/hello.txt", 5, 7, buf);
	REQUIRE(n == 5);
	buf[5] = 0;
	CHECK(strcmp(buf, "NanOS") == 0);

	// size past EOF gets clamped to the remaining bytes
	int m = fs.read("/hello.txt", 1000, 10, buf);
	CHECK(m == 8);                       // 18 - 10

	// offset at/beyond EOF returns 0 bytes (the Unix convention), not an error code —
	// so the syscall layer can propagate real read errors instead of masking them as EOF.
	CHECK(fs.read("/hello.txt", 1, 100, buf) == 0);
}

TEST_CASE("ext2 readdir lists the root directory") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	List<DirEntry> entries;
	REQUIRE(fs.readdir("/", entries) == 0);
	bool boot = false, hello = false;
	for (int i = 0; i < entries.getCount(); i++) {
		if (strcmp(entries[i].name, "boot") == 0) boot = true;
		if (strcmp(entries[i].name, "hello.txt") == 0) hello = true;
	}
	CHECK(boot);
	CHECK(hello);
}

TEST_CASE("ext2 follows a symbolic link to its target") {
	// /link.txt is a fast symlink -> /hello.txt (target stored inline in the inode).
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	char buf[64] = {0};
	int n = fs.read("/link.txt", sizeof(buf) - 1, 0, buf);
	REQUIRE(n > 0);
	buf[n] = 0;
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);   // resolved to the target's contents
	FileStat st;
	REQUIRE(fs.stat("/link.txt", st) == 0);
	CHECK(st.size == 18);                              // stat follows the link (target size)
}

TEST_CASE("ext2 lstat + readlink expose the symlink itself (no follow)") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	FileStat st;
	REQUIRE(fs.lstat("/link.txt", st) == 0);
	CHECK(st.type == NODE_SYMLINK);                 // lstat does NOT follow -> it's a link
	CHECK((st.mode & 0xF000) == 0xA000);            // S_IFLNK format bits
	char tgt[64] = {0};
	int n = fs.readlink("/link.txt", tgt, sizeof(tgt) - 1);
	REQUIRE(n > 0);
	tgt[n] = 0;
	CHECK(strcmp(tgt, "/hello.txt") == 0);          // the link's target path
	// readlink on a non-symlink is -EINVAL; lstat of a regular file is just a file.
	CHECK(fs.readlink("/hello.txt", tgt, sizeof(tgt)) == -22);
	FileStat s2;
	REQUIRE(fs.lstat("/hello.txt", s2) == 0);
	CHECK(s2.type == NODE_FILE);
}

TEST_CASE("ext2 reads a hard link as the same file") {
	// /hardhello.txt is a second directory entry for /hello.txt's inode.
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	char buf[64] = {0};
	int n = fs.read("/hardhello.txt", sizeof(buf) - 1, 0, buf);
	REQUIRE(n > 0);
	buf[n] = 0;
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);
}

TEST_CASE("ext2 readdir on a file (not a directory) errors") {
	Ext2Filesystem fs(loadFixture(), 0);
	fs.mount();
	List<DirEntry> entries;
	CHECK(fs.readdir("/hello.txt", entries) < 0);
}

TEST_CASE("ext4 type does not claim the ext2 image") {
	Ext4FileSystemType e4;
	CHECK(e4.probe(loadFixture(), 0) == false);
}

TEST_CASE("ext2 mounts through the VFS and reads by absolute path") {
	Vfs vfs;
	static Ext2FileSystemType ext2type;
	vfs.registerType(&ext2type);
	REQUIRE(vfs.mount("/", "ext2", loadFixture(), 0) == 0);

	char buf[64] = {0};
	int n = vfs.read("/hello.txt", sizeof(buf) - 1, 0, buf);
	REQUIRE(n > 0);
	buf[n] = 0;
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);
}
