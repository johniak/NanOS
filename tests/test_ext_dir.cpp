#include "doctest.h"
#include "Ext2Filesystem.h"
#include "Ext4Filesystem.h"
#include "RamBlockDevice.h"
#include <cstdio>
#include <cstring>
// malloc/free come from memory_manager.h (C++ linkage); do not include <cstdlib>.

using namespace kernel;

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
static bool hasEntry(ExtFilesystem& fs, const char* dir, const char* name) {
	List<DirEntry> e;
	if (fs.readdir((char*) dir, e) != 0) return false;
	for (int i = 0; i < e.getCount(); i++) if (strcmp(e[i].name, name) == 0) return true;
	return false;
}

// Runs the full namespace suite against a mounted ext filesystem, then dumps it for e2fsck.
static void exercise(ExtFilesystem& fs, const char* dumpPath) {
	FileStat st;

	// mkdir + the parent's link count grows for the new dir's "..".
	REQUIRE(fs.stat("/", st) == 0);
	unsigned rootLinks0 = st.nlink;
	CHECK(fs.mkdir("/d1", 0755) == 0);
	REQUIRE(fs.stat("/d1", st) == 0);
	CHECK(st.type == NODE_DIR);
	CHECK(st.nlink == 2);                               // "." + the name in root
	REQUIRE(fs.stat("/", st) == 0);
	CHECK(st.nlink == rootLinks0 + 1);

	// create + write + read back.
	CHECK(fs.create("/d1/f1", 0644) == 0);
	REQUIRE(fs.stat("/d1/f1", st) == 0);
	CHECK(st.type == NODE_FILE);
	CHECK(st.size == 0);
	const char* msg = "hello directory world";
	CHECK(fs.write("/d1/f1", (unsigned) strlen(msg), 0, msg) == (int) strlen(msg));
	char buf[64] = {0};
	CHECK(fs.read("/d1/f1", (unsigned) strlen(msg), 0, buf) == (int) strlen(msg));
	CHECK(strcmp(buf, msg) == 0);

	// hard link: same bytes via two names, link count 2.
	CHECK(fs.link("/d1/f1", "/d1/f1h") == 0);
	REQUIRE(fs.stat("/d1/f1h", st) == 0);
	CHECK(st.nlink == 2);
	memset(buf, 0, sizeof(buf));
	CHECK(fs.read("/d1/f1h", (unsigned) strlen(msg), 0, buf) == (int) strlen(msg));
	CHECK(strcmp(buf, msg) == 0);

	// symlink: lstat sees a link, readlink returns the target, stat follows it.
	CHECK(fs.symlink("/d1/f1", "/d1/sl") == 0);
	REQUIRE(fs.lstat("/d1/sl", st) == 0);
	CHECK(st.type == NODE_SYMLINK);
	char tgt[64] = {0};
	int tn = fs.readlink("/d1/sl", tgt, sizeof(tgt) - 1);
	REQUIRE(tn > 0);
	tgt[tn] = 0;
	CHECK(strcmp(tgt, "/d1/f1") == 0);

	// rename within a directory.
	CHECK(fs.create("/d1/f2", 0644) == 0);
	CHECK(fs.rename("/d1/f2", "/d1/f2r") == 0);
	CHECK(hasEntry(fs, "/d1", "f2r"));
	CHECK(!hasEntry(fs, "/d1", "f2"));

	// cross-directory rename of a subdirectory (its ".." must follow).
	CHECK(fs.mkdir("/d1/sub", 0755) == 0);
	CHECK(fs.mkdir("/d2", 0755) == 0);
	CHECK(fs.rename("/d1/sub", "/d2/sub") == 0);
	CHECK(hasEntry(fs, "/d2", "sub"));
	CHECK(!hasEntry(fs, "/d1", "sub"));
	REQUIRE(fs.stat("/d2/sub", st) == 0);
	CHECK(st.type == NODE_DIR);

	// unlink a hard link -> the other name's link count drops back to 1.
	CHECK(fs.unlink("/d1/f1h") == 0);
	CHECK(!hasEntry(fs, "/d1", "f1h"));
	REQUIRE(fs.stat("/d1/f1", st) == 0);
	CHECK(st.nlink == 1);

	// rmdir: must be empty.
	CHECK(fs.rmdir("/d2/sub") == 0);
	CHECK(fs.rmdir("/d2") == 0);
	CHECK(!hasEntry(fs, "/", "d2"));

	// Many entries -> the directory grows past one block (multi-block linear directory).
	for (int i = 0; i < 80; i++) {
		char p[32];
		int k = 0; const char* pre = "/d1/file";
		while (pre[k]) { p[k] = pre[k]; k++; }
		p[k++] = '0' + (i / 10); p[k++] = '0' + (i % 10); p[k] = 0;
		REQUIRE(fs.create(p, 0644) == 0);
	}
	REQUIRE(fs.stat("/d1", st) == 0);
	CHECK(st.size > 1024);                              // spilled into a second block
	CHECK(hasEntry(fs, "/d1", "file79"));

	// metadata mutations must stay e2fsck-clean.
	CHECK(fs.chmod("/d1/f1", 0600) == 0);
	REQUIRE(fs.stat("/d1/f1", st) == 0);
	CHECK((st.mode & 0xFFF) == 0600);
	CHECK(fs.chown("/d1/f1", 123, 456) == 0);
	REQUIRE(fs.stat("/d1/f1", st) == 0);
	CHECK(st.uid == 123);
	CHECK(st.gid == 456);
	CHECK(fs.utimes("/d1/f1", 1000, 2000) == 0);
	REQUIRE(fs.stat("/d1/f1", st) == 0);
	CHECK(st.mtime == 2000);

	StatFs sfs;
	CHECK(fs.statfs("/", sfs) == 0);
	CHECK(sfs.blockSize == 1024);
	CHECK(sfs.totalBlocks > 0);
	CHECK(sfs.nameMax == 255);
	(void) dumpPath;
}

TEST_CASE("ext2 directory operations stay e2fsck-clean") {
	char* img; long sz;
	Ext2Filesystem fs(load("tests/fixtures/ext2.img", &img, &sz), 0);
	REQUIRE(fs.mount() == 0);
	exercise(fs, nullptr);
	dump("/src/disk/ext2_dir.img", img, sz);
}

TEST_CASE("ext4 directory operations stay e2fsck-clean") {
	char* img; long sz;
	Ext4Filesystem fs(load("tests/fixtures/ext4.img", &img, &sz), 0);
	REQUIRE(fs.mount() == 0);
	exercise(fs, nullptr);
	dump("/src/disk/ext4_dir.img", img, sz);
}
