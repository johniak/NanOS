#include "doctest.h"
#include "RamFs.h"
#include <cstring>

using namespace kernel;

TEST_CASE("RamFs root mode: default 0755, /tmp mounts 01777 (sticky, world-writable)") {
	RamFs def;                       // default mount
	FileStat st;
	CHECK(def.stat(String("/"), st) == 0);
	CHECK((st.mode & 07777) == 0755);
	RamFs tmp(01777);                // the /tmp mount
	CHECK(tmp.stat(String("/"), st) == 0);
	CHECK((st.mode & 07777) == 01777);   // sticky bit + rwx for all -> non-root can create temp files
}

TEST_CASE("RamFs create/write/read round-trips a file") {
	RamFs fs;
	CHECK(fs.mount() == 0);
	// Missing file reads/stats as -ENOENT until created.
	FileStat st;
	CHECK(fs.stat(String("/a.txt"), st) == -2);
	CHECK(fs.create(String("/a.txt"), 0644) == 0);
	CHECK(fs.stat(String("/a.txt"), st) == 0);
	CHECK(st.type == NODE_FILE);
	CHECK(st.size == 0);

	const char* msg = "hello tmpfs";
	CHECK(fs.write(String("/a.txt"), (unsigned) strlen(msg), 0, msg) == (int) strlen(msg));
	CHECK(fs.stat(String("/a.txt"), st) == 0);
	CHECK(st.size == strlen(msg));

	char buf[32] = {0};
	CHECK(fs.read(String("/a.txt"), 32, 0, buf) == (int) strlen(msg));
	CHECK(strcmp(buf, msg) == 0);
	// Read at an offset.
	CHECK(fs.read(String("/a.txt"), 32, 6, buf) == 5);   // "tmpfs"
	CHECK(strncmp(buf, "tmpfs", 5) == 0);
}

TEST_CASE("RamFs write grows the file and offset writes zero-fill the gap") {
	RamFs fs;
	fs.create(String("/g.bin"), 0644);
	// Write 4 bytes at offset 8 -> bytes 0..7 are a zero-filled gap, size becomes 12.
	const char* tail = "ABCD";
	CHECK(fs.write(String("/g.bin"), 4, 8, tail) == 4);
	FileStat st;
	fs.stat(String("/g.bin"), st);
	CHECK(st.size == 12);
	unsigned char buf[16];
	CHECK(fs.read(String("/g.bin"), 16, 0, buf) == 12);
	for (int i = 0; i < 8; i++)
		CHECK(buf[i] == 0);
	CHECK(memcmp(buf + 8, "ABCD", 4) == 0);
}

TEST_CASE("RamFs create truncates an existing file") {
	RamFs fs;
	fs.create(String("/t.txt"), 0644);
	fs.write(String("/t.txt"), 5, 0, "12345");
	CHECK(fs.create(String("/t.txt"), 0644) == 0);   // make-or-truncate
	FileStat st;
	fs.stat(String("/t.txt"), st);
	CHECK(st.size == 0);
}

TEST_CASE("RamFs mknod: AF_UNIX socket node (S_IFSOCK), EEXIST on rebind") {
	RamFs fs;
	CHECK(fs.mknod(String("/s.sock"), 0xC000 | 0777) == 0);
	FileStat st;
	REQUIRE(fs.stat(String("/s.sock"), st) == 0);
	CHECK((st.mode & 0xF000u) == 0xC000u);            // S_IFSOCK
	CHECK(st.type == NODE_OTHER);
	CHECK(st.size == 0);
	CHECK(fs.mknod(String("/s.sock"), 0xC000 | 0777) == -17);   // EEXIST: must unlink to rebind
	CHECK(fs.unlink(String("/s.sock")) == 0);
	CHECK(fs.mknod(String("/s.sock"), 0xC000 | 0777) == 0);     // rebind after unlink
}

TEST_CASE("RamFs mkdir + nested files, readdir lists . .. and children") {
	RamFs fs;
	CHECK(fs.mkdir(String("/sub"), 0755) == 0);
	CHECK(fs.mkdir(String("/sub"), 0755) == -17);     // -EEXIST
	CHECK(fs.create(String("/sub/inner.txt"), 0644) == 0);
	FileStat st;
	CHECK(fs.stat(String("/sub"), st) == 0);
	CHECK(st.type == NODE_DIR);
	CHECK(fs.stat(String("/sub/inner.txt"), st) == 0);
	CHECK(st.type == NODE_FILE);

	List<DirEntry> entries;
	CHECK(fs.readdir(String("/sub"), entries) == 0);
	// "." "..", then inner.txt
	bool dot = false, dotdot = false, inner = false;
	for (int i = 0; i < entries.getCount(); i++) {
		if (!strcmp(entries[i].name, ".")) dot = true;
		if (!strcmp(entries[i].name, "..")) dotdot = true;
		if (!strcmp(entries[i].name, "inner.txt")) inner = true;
	}
	CHECK(dot);
	CHECK(dotdot);
	CHECK(inner);

	// Creating a file under a missing parent fails.
	CHECK(fs.create(String("/nope/x.txt"), 0644) == -2);
}

TEST_CASE("RamFs type-mismatch and large-growth paths") {
	RamFs fs;
	fs.mkdir(String("/dir"), 0755);
	char buf[8];
	CHECK(fs.read(String("/dir"), 8, 0, buf) == -21);        // -EISDIR: read on a dir
	CHECK(fs.write(String("/dir"), 1, 0, "x") == -21);       // -EISDIR: write on a dir
	fs.create(String("/f"), 0644);
	List<DirEntry> e;
	CHECK(fs.readdir(String("/f"), e) == -20);               // -ENOTDIR: readdir on a file
	CHECK(fs.create(String("/dir"), 0644) == -21);           // create over an existing dir
	// A write past the initial 64-byte capacity exercises the geometric realloc growth.
	char big[300];
	for (int i = 0; i < 300; i++) big[i] = (char) (i & 0x7F);
	CHECK(fs.write(String("/f"), 300, 0, big) == 300);
	FileStat st;
	fs.stat(String("/f"), st);
	CHECK(st.size == 300);
	char back[300];
	CHECK(fs.read(String("/f"), 300, 0, back) == 300);
	CHECK(memcmp(big, back, 300) == 0);
	// Reads/writes/readdir on a wholly missing path.
	CHECK(fs.read(String("/none"), 8, 0, buf) == -2);
	CHECK(fs.write(String("/none"), 1, 0, "x") == -2);
	CHECK(fs.readdir(String("/none"), e) == -2);
}

TEST_CASE("RamFs honors the create/mkdir mode instead of a hardcoded permission") {
	RamFs fs;
	CHECK(fs.mount() == 0);
	FileStat st;
	CHECK(fs.create(String("/secret"), 0600) == 0);
	CHECK(fs.stat(String("/secret"), st) == 0);
	CHECK(st.type == NODE_FILE);
	CHECK((st.mode & 0xF000) == 0x8000);   // S_IFREG
	CHECK((st.mode & 0777) == 0600);       // the requested perms, not a fixed 0666

	CHECK(fs.mkdir(String("/priv"), 0700) == 0);
	CHECK(fs.stat(String("/priv"), st) == 0);
	CHECK((st.mode & 0xF000) == 0x4000);   // S_IFDIR
	CHECK((st.mode & 0777) == 0700);       // not a fixed 0755
}

TEST_CASE("RamFs grows directory children past the old 32 cap — no silent -ENOSPC") {
	RamFs fs;
	CHECK(fs.mount() == 0);
	const int N = 100;                       // well past the former fixed child[32]
	for (int i = 0; i < N; i++) {
		char p[32];                          // build "/f<i>" without stdio
		p[0] = '/'; p[1] = 'f';
		int k = 2, v = i, d = 0;
		char tmp[8];
		do { tmp[d++] = (char) ('0' + v % 10); v /= 10; } while (v);
		while (d) p[k++] = tmp[--d];
		p[k] = 0;
		CHECK(fs.create(String(p), 0644) == 0);   // every create must succeed
	}
	List<DirEntry> ents;
	CHECK(fs.readdir(String("/"), ents) == 0);
	int files = 0;
	for (int i = 0; i < ents.getCount(); i++)
		if (ents[i].name[0] == 'f')
			files++;
	CHECK(files == N);                       // readdir enumerates all of them
}

TEST_CASE("RamFs unlink removes a file; dirs and missing names are rejected") {
	RamFs fs;
	fs.create(String("/del.txt"), 0644);
	FileStat st;
	CHECK(fs.stat(String("/del.txt"), st) == 0);
	CHECK(fs.unlink(String("/del.txt")) == 0);
	CHECK(fs.stat(String("/del.txt"), st) == -2);     // gone
	CHECK(fs.unlink(String("/del.txt")) == -2);       // already gone
	fs.mkdir(String("/d"), 0755);
	CHECK(fs.unlink(String("/d")) == -21);            // -EISDIR: unlink is for files
}

TEST_CASE("RamFs rmdir/rename/link/symlink/chmod/chown/utimes/truncate/statfs parity") {
	RamFs fs;
	REQUIRE(fs.mount() == 0);

	// rmdir: empty only.
	CHECK(fs.mkdir(String("/d"), 0755) == 0);
	CHECK(fs.create(String("/d/f"), 0644) == 0);
	CHECK(fs.rmdir(String("/d")) == -39);             // -ENOTEMPTY
	CHECK(fs.unlink(String("/d/f")) == 0);
	CHECK(fs.rmdir(String("/d")) == 0);
	FileStat st;
	CHECK(fs.stat(String("/d"), st) == -2);

	// chmod / chown / utimes.
	CHECK(fs.create(String("/m"), 0644) == 0);
	CHECK(fs.chmod(String("/m"), 0600) == 0);
	REQUIRE(fs.stat(String("/m"), st) == 0);
	CHECK((st.mode & 0777) == 0600);
	CHECK(fs.chown(String("/m"), 11, 22) == 0);
	REQUIRE(fs.stat(String("/m"), st) == 0);
	CHECK(st.uid == 11); CHECK(st.gid == 22);
	CHECK(fs.utimes(String("/m"), 100, 200) == 0);
	REQUIRE(fs.stat(String("/m"), st) == 0);
	CHECK(st.mtime == 200);

	// truncate (grow with zero-fill, then shrink).
	CHECK(fs.write(String("/m"), 3, 0, "abc") == 3);
	CHECK(fs.truncate(String("/m"), 5) == 0);
	REQUIRE(fs.stat(String("/m"), st) == 0);
	CHECK(st.size == 5);
	char rb[8] = {0};
	CHECK(fs.read(String("/m"), 5, 0, rb) == 5);
	CHECK(rb[3] == 0); CHECK(rb[4] == 0);
	CHECK(fs.truncate(String("/m"), 2) == 0);
	REQUIRE(fs.stat(String("/m"), st) == 0);
	CHECK(st.size == 2);

	// hard link: same content via two names, nlink 2; unlink one keeps the other.
	CHECK(fs.write(String("/m"), 2, 0, "hi") == 2);
	CHECK(fs.link(String("/m"), "/m2") == 0);
	REQUIRE(fs.stat(String("/m2"), st) == 0);
	CHECK(st.nlink == 2);
	CHECK(fs.read(String("/m2"), 2, 0, rb) == 2);
	CHECK(rb[0] == 'h');
	CHECK(fs.unlink(String("/m")) == 0);
	REQUIRE(fs.stat(String("/m2"), st) == 0);          // still here
	CHECK(st.nlink == 1);

	// symlink + readlink + lstat; stat follows it.
	CHECK(fs.symlink("/m2", "/sl") == 0);
	REQUIRE(fs.lstat(String("/sl"), st) == 0);
	CHECK(st.type == NODE_SYMLINK);
	char tg[8] = {0};
	CHECK(fs.readlink(String("/sl"), tg, sizeof(tg) - 1) == 3);
	CHECK(strcmp(tg, "/m2") == 0);
	REQUIRE(fs.stat(String("/sl"), st) == 0);          // stat follows -> the target file
	CHECK(st.type == NODE_FILE);
	CHECK(fs.read(String("/sl"), 2, 0, rb) == 2);      // read through the link

	// rename within the root.
	CHECK(fs.rename(String("/m2"), "/m3") == 0);
	CHECK(fs.stat(String("/m2"), st) == -2);
	REQUIRE(fs.stat(String("/m3"), st) == 0);

	// statfs.
	StatFs sfs;
	CHECK(fs.statfs(String("/"), sfs) == 0);
	CHECK(sfs.blockSize == 4096);
	CHECK(sfs.nameMax == 63);
}
