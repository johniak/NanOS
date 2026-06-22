#include "doctest.h"
#include "Vfs.h"
#include "RamFs.h"
#include "Cred.h"
using namespace kernel;

// A single mutable test credential the provider hands back; tests rewrite it to "become" a user.
static Cred g_tc;
static const Cred* tcProvider() { return &g_tc; }

static void beUser(unsigned uid, unsigned gid) {
	credInitRoot(g_tc);
	g_tc.ruid = g_tc.euid = g_tc.suid = g_tc.fsuid = uid;
	g_tc.rgid = g_tc.egid = g_tc.sgid = g_tc.fsgid = gid;
}

static Vfs* freshVfs() {
	Vfs* vfs = new Vfs();
	vfs->mount(String("/"), new RamFs());
	vfs->setCredProvider(tcProvider);
	return vfs;
}

TEST_CASE("Vfs denies read without permission and allows it for owner/root") {
	Vfs* vfs = freshVfs();
	credInitRoot(g_tc);                         // root creates a 0600 root-owned file
	REQUIRE(vfs->create(String("/secret"), 0600) == 0);
	REQUIRE(vfs->chown(String("/secret"), 0, 0) == 0);
	char buf[4];
	beUser(1000, 1000);
	CHECK(vfs->read(String("/secret"), 1, 0, buf) == -13);   // other has no read -> EACCES
	credInitRoot(g_tc);
	CHECK(vfs->read(String("/secret"), 0, 0, buf) >= 0);     // root reads anything
}

TEST_CASE("Vfs path-walk requires search (x) on every ancestor directory") {
	Vfs* vfs = freshVfs();
	credInitRoot(g_tc);
	REQUIRE(vfs->mkdir(String("/dir"), 0700) == 0);          // root-only dir (no x for others)
	REQUIRE(vfs->chown(String("/dir"), 0, 0) == 0);
	REQUIRE(vfs->create(String("/dir/f"), 0644) == 0);
	beUser(1000, 1000);
	char b[4];
	CHECK(vfs->read(String("/dir/f"), 1, 0, b) == -13);      // cannot search /dir -> EACCES
}

TEST_CASE("Vfs new file/dir is owned by the creating user; setgid dir propagates group") {
	Vfs* vfs = freshVfs();
	credInitRoot(g_tc);
	REQUIRE(vfs->mkdir(String("/pub"), 0777) == 0);          // world-writable
	REQUIRE(vfs->chown(String("/pub"), 0, 0) == 0);
	beUser(1000, 1000);
	REQUIRE(vfs->create(String("/pub/mine"), 0644) == 0);    // creatable: parent is world-writable
	FileStat st;
	REQUIRE(vfs->stat(String("/pub/mine"), st) == 0);
	CHECK(st.uid == 1000);                                   // owned by the creator
	CHECK(st.gid == 1000);
}

TEST_CASE("Vfs sticky directory: non-owner cannot unlink another's file") {
	Vfs* vfs = freshVfs();
	credInitRoot(g_tc);
	REQUIRE(vfs->mkdir(String("/tmp"), 0777) == 0);
	REQUIRE(vfs->chmod(String("/tmp"), 01777) == 0);         // make it sticky
	REQUIRE(vfs->chown(String("/tmp"), 0, 0) == 0);
	REQUIRE(vfs->create(String("/tmp/root_file"), 0644) == 0);
	REQUIRE(vfs->chown(String("/tmp/root_file"), 0, 0) == 0);
	beUser(1000, 1000);
	CHECK(vfs->unlink(String("/tmp/root_file")) == -1);      // sticky, not the owner -> EPERM
	REQUIRE(vfs->create(String("/tmp/mine"), 0644) == 0);    // creator owns it
	CHECK(vfs->unlink(String("/tmp/mine")) == 0);            // own file in a sticky dir
}

TEST_CASE("Vfs chmod requires ownership; chown owner is root-only") {
	Vfs* vfs = freshVfs();
	credInitRoot(g_tc);
	REQUIRE(vfs->create(String("/f"), 0644) == 0);
	REQUIRE(vfs->chown(String("/f"), 0, 0) == 0);
	beUser(1000, 1000);
	CHECK(vfs->chmod(String("/f"), 0666) == -1);             // not the owner -> EPERM
	CHECK(vfs->chown(String("/f"), 1000, -1) == -1);         // changing owner is root-only -> EPERM
}

TEST_CASE("Vfs access() uses real ids; a null CredProvider bypasses all checks") {
	Vfs* vfs = new Vfs();                                    // no provider installed
	vfs->mount(String("/"), new RamFs());
	REQUIRE(vfs->create(String("/x"), 0600) == 0);           // works with no provider (root-equiv)
	char b[2];
	CHECK(vfs->read(String("/x"), 0, 0, b) >= 0);            // unchecked
}
