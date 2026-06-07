#include "doctest.h"
#include "SynthFs.h"
#include <cstring>

using namespace kernel;

static bool listed(List<DirEntry>& e, const char* name) {
	for (int i = 0; i < e.getCount(); i++)
		if (strcmp(e[i].name, name) == 0)
			return true;
	return false;
}

TEST_CASE("SynthFs root lists the virtual top-level dirs") {
	SynthFs fs;
	List<DirEntry> e;
	REQUIRE(fs.readdir("/", e) == 0);
	CHECK(listed(e, "disks"));
	CHECK(listed(e, "dev"));
	CHECK(listed(e, "proc"));
}

TEST_CASE("SynthFs stat: virtual dirs are directories") {
	SynthFs fs;
	FileStat st;
	REQUIRE(fs.stat("/", st) == 0);
	CHECK(st.type == NODE_DIR);
	REQUIRE(fs.stat("/dev", st) == 0);
	CHECK(st.type == NODE_DIR);
}

TEST_CASE("SynthFs static node: read returns its bytes, honours offset/size") {
	SynthFs fs;
	fs.addStatic(fs.proc(), "version", "NanOS v0\n", 9);

	FileStat st;
	REQUIRE(fs.stat("/proc/version", st) == 0);
	CHECK(st.type == NODE_FILE);
	CHECK(st.size == 9);

	char buf[16] = {0};
	CHECK(fs.read("/proc/version", 9, 0, buf) == 9);
	CHECK(strncmp(buf, "NanOS v0\n", 9) == 0);

	char p[8] = {0};
	CHECK(fs.read("/proc/version", 3, 6, p) == 3);   // bytes [6,9) = "v0\n"
	CHECK(strncmp(p, "v0\n", 3) == 0);
}

TEST_CASE("SynthFs addVolume shows up under /disks") {
	SynthFs fs;
	List<DirEntry> e0;
	REQUIRE(fs.readdir("/disks", e0) == 0);
	CHECK(!listed(e0, "main"));

	fs.addVolume("main");
	List<DirEntry> e1;
	REQUIRE(fs.readdir("/disks", e1) == 0);
	CHECK(listed(e1, "main"));
}

TEST_CASE("SynthFs errors on missing paths and bad ops") {
	SynthFs fs;
	FileStat st;
	CHECK(fs.stat("/nope", st) < 0);
	char b[4];
	CHECK(fs.read("/nope", 1, 0, b) < 0);
	List<DirEntry> e;
	CHECK(fs.readdir("/proc/version-missing", e) < 0);
}
