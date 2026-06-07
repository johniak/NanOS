#include "doctest.h"
#include "Syscall.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "RamBlockDevice.h"
#include <cstdio>
#include <cstring>
// malloc/free via memory_manager.h (transitive); do not include <cstdlib>.

using namespace kernel;

static char g_out[8192];
static unsigned g_outLen;
static int sink(const char* b, unsigned n) {
	for (unsigned i = 0; i < n; i++)
		if (g_outLen < sizeof(g_out))
			g_out[g_outLen++] = b[i];
	return (int) n;
}

static Vfs* mountFixture() {
	FILE* f = fopen("tests/fixtures/ext2.img", "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = (char*) malloc(sz);
	REQUIRE(fread(buf, 1, sz, f) == (size_t) sz);
	fclose(f);
	RamBlockDevice* dev = new RamBlockDevice("d", buf, (unsigned) sz);
	Vfs* vfs = new Vfs();
	static Ext2FileSystemType t;
	vfs->registerType(&t);
	REQUIRE(vfs->mount("/", "ext2", dev, 0) == 0);
	return vfs;
}

TEST_CASE("sys_write to fd 1 reaches the console sink") {
	g_outLen = 0;
	Syscalls sc(mountFixture(), sink);
	CHECK(sc.write(1, "hi!", 3) == 3);
	CHECK(g_outLen == 3);
	CHECK(strncmp(g_out, "hi!", 3) == 0);
}

TEST_CASE("sys_open/read returns file bytes and advances the offset") {
	Syscalls sc(mountFixture(), sink);
	int fd = sc.open("/hello.txt", 0);
	REQUIRE(fd >= 3);
	char buf[64] = {0};
	CHECK(sc.read(fd, buf, 64) == 18);
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);
	CHECK(sc.read(fd, buf, 64) == 0);          // at EOF, offset advanced
}

TEST_CASE("sys_lseek repositions and read continues from there") {
	Syscalls sc(mountFixture(), sink);
	int fd = sc.open("/hello.txt", 0);
	CHECK(sc.lseek(fd, 7, 0 /*SEEK_SET*/) == 7);
	char buf[8] = {0};
	CHECK(sc.read(fd, buf, 5) == 5);
	CHECK(strncmp(buf, "NanOS", 5) == 0);
}

TEST_CASE("sys errors: bad fd, missing path, write to read-only file") {
	Syscalls sc(mountFixture(), sink);
	char buf[8];
	CHECK(sc.read(99, buf, 8) == -9);          // -EBADF
	CHECK(sc.open("/does-not-exist", 0) == -2); // -ENOENT
	int fd = sc.open("/hello.txt", 0);
	CHECK(sc.write(fd, "x", 1) == -30);        // -EROFS (file, read-only)
	CHECK(sc.close(fd) == 0);
	CHECK(sc.read(fd, buf, 8) == -9);          // closed -> -EBADF
}

TEST_CASE("sys_fstat reports size and file type") {
	Syscalls sc(mountFixture(), sink);
	int fd = sc.open("/hello.txt", 0);
	LinuxStat st;
	CHECK(sc.fstat(fd, &st) == 0);
	CHECK(st.st_size == 18);
	CHECK((st.st_mode & 0xF000) == 0x8000);    // S_IFREG
	CHECK(st.st_nlink >= 1);
}

TEST_CASE("sys_stat by path reports metadata; missing path -> -ENOENT") {
	Syscalls sc(mountFixture(), sink);
	LinuxStat st;
	CHECK(sc.stat(String((char*) "/hello.txt"), &st) == 0);
	CHECK(st.st_size == 18);
	CHECK((st.st_mode & 0xF000) == 0x8000);    // S_IFREG
	CHECK(st.st_nlink >= 1);

	LinuxStat dst;
	CHECK(sc.stat(String((char*) "/boot"), &dst) == 0);
	CHECK((dst.st_mode & 0xF000) == 0x4000);   // S_IFDIR

	CHECK(sc.stat(String((char*) "/no/such"), &st) == -2);   // -ENOENT
}

TEST_CASE("sys_getdents64 lists a directory") {
	Syscalls sc(mountFixture(), sink);
	int fd = sc.open("/boot/grub", 0);
	REQUIRE(fd >= 3);
	char buf[512];
	int n = sc.getdents64(fd, buf, sizeof(buf));
	REQUIRE(n > 0);
	// the names appear inline in the dirent records
	bool found = false;
	for (int i = 0; i + 8 < n; i++)
		if (strncmp(buf + i, "grub.cfg", 8) == 0)
			found = true;
	CHECK(found);
}

TEST_CASE("sys_lseek SEEK_CUR/SEEK_END and bad whence/offset") {
	Syscalls sc(mountFixture(), sink);
	int fd = sc.open("/hello.txt", 0);          // size 18
	CHECK(sc.lseek(fd, 5, SEEK_SET) == 5);
	CHECK(sc.lseek(fd, 3, SEEK_CUR) == 8);
	CHECK(sc.lseek(fd, -2, SEEK_END) == 16);    // 18 - 2
	CHECK(sc.lseek(fd, 0, 99) == -22);          // -EINVAL bad whence
	CHECK(sc.lseek(fd, -100, SEEK_SET) == -22); // -EINVAL negative
	CHECK(sc.lseek(99, 0, SEEK_SET) == -9);     // -EBADF
}

TEST_CASE("sys_fstat on a console fd and bad fd") {
	Syscalls sc(mountFixture(), sink);
	LinuxStat st;
	CHECK(sc.fstat(1, &st) == 0);
	CHECK((st.st_mode & 0xF000) == 0x2000);     // S_IFCHR
	CHECK(sc.fstat(99, &st) == -9);             // -EBADF
}

TEST_CASE("sys close/getdents errors") {
	Syscalls sc(mountFixture(), sink);
	CHECK(sc.close(99) == -9);                  // -EBADF
	char buf[64];
	CHECK(sc.getdents64(99, buf, sizeof(buf)) == -9);   // -EBADF
}

TEST_CASE("sys_exit records the exit code") {
	Syscalls sc(mountFixture(), sink);
	CHECK(sc.hasExited() == false);
	sc.exit(7);
	CHECK(sc.hasExited() == true);
	CHECK(sc.code() == 7);
}

TEST_CASE("resetForRun clears the exit state so the instance can run again") {
	Syscalls sc(mountFixture(), sink);
	sc.exit(7);
	CHECK(sc.hasExited() == true);
	sc.resetForRun();
	CHECK(sc.hasExited() == false);
	CHECK(sc.code() == 0);
}
