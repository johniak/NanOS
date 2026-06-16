#include "doctest.h"
#include "Vfs.h"
#include "RamFs.h"
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
	// Records the relative path; returns -EROFS like the FileSystem default (so the read-only
	// assertions elsewhere still hold) while letting tests see whether mkdir was routed at all.
	int mkdir(String path, unsigned) {
		strcpy(received, (char*) path);
		return -30;
	}
};

struct FakeType : FileSystemType {
	const char* tn;
	FakeFS* made;
	bool probeResult;
	FakeType(const char* n) : tn(n), made(0), probeResult(true) {}
	const char* name() { return tn; }
	bool probe(BlockDevice*, unsigned) { return probeResult; }
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

TEST_CASE("Vfs rejects an over-long mountpoint instead of truncating it") {
	Vfs vfs;
	FakeType t("rootfs");
	vfs.registerType(&t);
	// 300 chars > the 256-byte mountpoint buffer: must be rejected, not silently cut to a
	// shorter prefix that would then misroute every path under it.
	char longmp[302];
	longmp[0] = '/';
	for (int i = 1; i < 300; i++) longmp[i] = 'a';
	longmp[300] = 0;
	CHECK(vfs.mount(String(longmp), "rootfs", (BlockDevice*)0, 0) == -36);   // -ENAMETOOLONG
	// A path under the (rejected) mountpoint resolves to nothing, not a truncated mount.
	List<DirEntry> e;
	CHECK(vfs.readdir(String(longmp), e) < 0);
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
		bool probe(BlockDevice*, unsigned) { return true; }
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
		bool probe(BlockDevice*, unsigned) { return true; }
		FileSystem* create(BlockDevice*, unsigned) { return 0; }
	};
	Vfs vfs;
	NullType t;
	vfs.registerType(&t);
	CHECK(vfs.mount("/", "nullfs", (BlockDevice*)0, 0) < 0);
}

TEST_CASE("Vfs auto-mount picks the first type whose probe matches") {
	Vfs vfs;
	FakeType a("afs"); a.probeResult = false;
	FakeType b("bfs"); b.probeResult = true;
	vfs.registerType(&a);
	vfs.registerType(&b);
	CHECK(vfs.mount("/", "auto", (BlockDevice*)0, 0) == 0);
	CHECK(b.made != 0);     // b matched and was created
	CHECK(a.made == 0);     // a was skipped (probe false)
}

TEST_CASE("Vfs auto-mount fails when no type probes true") {
	Vfs vfs;
	FakeType a("afs"); a.probeResult = false;
	vfs.registerType(&a);
	CHECK(vfs.mount("/", "auto", (BlockDevice*)0, 0) < 0);
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

TEST_CASE("Vfs mounts a pre-built FileSystem (no device/type) and routes to it") {
	Vfs vfs;
	FakeFS root(7);
	CHECK(vfs.mount("/", &root) == 0);
	CHECK(root.mountCalls == 1);          // overload calls fs->mount() once
	char buf[8];
	CHECK(vfs.read("/dev/random", 1, 0, buf) == 7);
	CHECK(strcmp(root.received, "/dev/random") == 0);
}

TEST_CASE("Vfs routes writes to a tmpfs mount; a read-only mount stays -EROFS") {
	Vfs vfs;
	FakeFS root(1);                  // read-only fs (no write/create override)
	RamFs tmp;
	CHECK(vfs.mount("/", &root) == 0);
	CHECK(vfs.mount("/tmp", &tmp) == 0);

	// A write/create under /tmp lands in the tmpfs (longest-prefix routing strips /tmp).
	CHECK(vfs.create("/tmp/save.dsg", 0644) == 0);
	const char* data = "savegame";
	CHECK(vfs.write("/tmp/save.dsg", 8, 0, data) == 8);
	char buf[16] = {0};
	CHECK(vfs.read("/tmp/save.dsg", 16, 0, buf) == 8);
	CHECK(strncmp(buf, "savegame", 8) == 0);
	CHECK(vfs.mkdir("/tmp/d", 0755) == 0);

	// The read-only root rejects writes/creates with -EROFS (the FileSystem default).
	CHECK(vfs.write("/etc/x", 1, 0, data) == -30);
	CHECK(vfs.create("/etc/x", 0644) == -30);
	CHECK(vfs.mkdir("/etc", 0755) == -30);
}

TEST_CASE("Vfs reports EEXIST for mkdir of a mount-point root (lets mkdir -p cross mounts)") {
	Vfs vfs;
	FakeFS root(1);
	FakeFS sub(2);
	CHECK(vfs.mount("/", &root) == 0);
	CHECK(vfs.mount("/disks", &sub) == 0);
	// mkdir of the mount-point root itself: EEXIST, and the mounted fs's mkdir is NOT called
	// (so `mkdir -p /disks/main/...` walks past /disks instead of aborting on EROFS/ENOENT).
	sub.received[0] = 0;
	CHECK(vfs.mkdir("/disks", 0755) == -17);   // -EEXIST
	CHECK(sub.received[0] == 0);               // routing short-circuited before fs->mkdir
	// A path below the mount still routes normally, with the mountpoint stripped.
	CHECK(vfs.mkdir("/disks/new", 0755) == -30);
	CHECK(strcmp(sub.received, "/new") == 0);
}
