#include "doctest.h"
#include "RamFs.h"
#include <cstring>

using namespace kernel;

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
