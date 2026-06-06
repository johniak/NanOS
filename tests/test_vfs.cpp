#include "doctest.h"
#include "Vfs.h"
#include <cstring>

using namespace kernel;

namespace {

// Records the relative path it was handed, so routing can be asserted.
struct FakeFS : FileSystem {
	int id;
	int mountCalls;
	char received[256];
	FakeFS(int i) : id(i), mountCalls(0) { received[0] = 0; }
	int mount() { mountCalls++; return 0; }
	int read(String path, unsigned, unsigned, void*) {
		strcpy(received, (char*) path);
		return id;
	}
	int stat(String path, FileStat& out) {
		strcpy(received, (char*) path);
		out.type = NODE_FILE; out.size = 0;
		return 0;
	}
	int readdir(String path, List<DirEntry>&) {
		strcpy(received, (char*) path);
		return 0;
	}
};

struct FakeType : FileSystemType {
	const char* tn;
	FakeFS* made;
	FakeType(const char* n) : tn(n), made(0) {}
	const char* name() { return tn; }
	FileSystem* create(BlockDevice*, unsigned) { made = new FakeFS(1); return made; }
};

}

TEST_CASE("Vfs rejects mount of an unregistered filesystem type") {
	Vfs vfs;
	CHECK(vfs.mount("/", "nofs", (BlockDevice*)0, 0) < 0);
}

TEST_CASE("Vfs mounts a registered type and calls its mount()") {
	Vfs vfs;
	FakeType t("rootfs");
	vfs.registerType(&t);
	CHECK(vfs.mount("/", "rootfs", (BlockDevice*)0, 2048) == 0);
	REQUIRE(t.made != 0);
	CHECK(t.made->mountCalls == 1);
}

TEST_CASE("Vfs errors when no mount matches the path") {
	Vfs vfs;
	char dummy[8];
	CHECK(vfs.read("/x", 1, 0, dummy) < 0);
	FileStat st;
	CHECK(vfs.stat("/x", st) < 0);
}

TEST_CASE("Vfs propagates a filesystem's mount() failure") {
	struct FailFS : FileSystem {
		int mount() { return -5; }
		int read(String, unsigned, unsigned, void*) { return 0; }
		int stat(String, FileStat&) { return 0; }
		int readdir(String, List<DirEntry>&) { return 0; }
	};
	struct FailType : FileSystemType {
		const char* name() { return "failfs"; }
		FileSystem* create(BlockDevice*, unsigned) { return new FailFS(); }
	};
	Vfs vfs;
	FailType t;
	vfs.registerType(&t);
	CHECK(vfs.mount("/", "failfs", (BlockDevice*)0, 0) == -5);
}

TEST_CASE("Vfs fails the mount when the type cannot create a filesystem") {
	struct NullType : FileSystemType {
		const char* name() { return "nullfs"; }
		FileSystem* create(BlockDevice*, unsigned) { return 0; }
	};
	Vfs vfs;
	NullType t;
	vfs.registerType(&t);
	CHECK(vfs.mount("/", "nullfs", (BlockDevice*)0, 0) < 0);
}

TEST_CASE("Vfs routes readdir to the mounted filesystem") {
	Vfs vfs;
	FakeType t("rootfs");
	vfs.registerType(&t);
	vfs.mount("/", "rootfs", (BlockDevice*)0, 0);
	List<DirEntry> out;
	CHECK(vfs.readdir("/some/dir", out) == 0);
	CHECK(strcmp(t.made->received, "/some/dir") == 0);
}

TEST_CASE("Vfs routes by longest-prefix mountpoint, stripping it") {
	Vfs vfs;
	FakeType tRoot("rootfs");
	FakeType tDev("devfs");
	vfs.registerType(&tRoot);
	vfs.registerType(&tDev);
	vfs.mount("/", "rootfs", (BlockDevice*)0, 0);
	vfs.mount("/dev", "devfs", (BlockDevice*)0, 0);

	char buf[8];
	// "/dev/tty" -> devfs as "/tty"
	CHECK(vfs.read("/dev/tty", 1, 0, buf) == 1);
	CHECK(strcmp(tDev.made->received, "/tty") == 0);

	// "/boot/x" -> rootfs as "/boot/x" (does not match "/dev")
	vfs.read("/boot/x", 1, 0, buf);
	CHECK(strcmp(tRoot.made->received, "/boot/x") == 0);

	// "/device" must NOT match the "/dev" mount (boundary check) -> rootfs
	vfs.read("/device", 1, 0, buf);
	CHECK(strcmp(tRoot.made->received, "/device") == 0);

	// exact "/dev" -> devfs as "/"
	vfs.read("/dev", 1, 0, buf);
	CHECK(strcmp(tDev.made->received, "/") == 0);
}
