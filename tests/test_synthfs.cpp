#include "doctest.h"
#include "SynthFs.h"
#include "Process.h"
#include <cstring>
#include <cstdio>

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

TEST_CASE("SynthFs /dev generators: null/zero/random") {
	SynthFs fs;
	List<DirEntry> e;
	REQUIRE(fs.readdir("/dev", e) == 0);
	CHECK(listed(e, "null"));
	CHECK(listed(e, "zero"));
	CHECK(listed(e, "random"));

	char z[16];
	memset(z, 0xAB, sizeof z);
	CHECK(fs.read("/dev/zero", 16, 0, z) == 16);
	bool allzero = true;
	for (int i = 0; i < 16; i++) if (z[i] != 0) allzero = false;
	CHECK(allzero);

	char nb[4];
	CHECK(fs.read("/dev/null", 4, 0, nb) == 0);   // EOF

	char r1[8], r2[8];
	CHECK(fs.read("/dev/random", 8, 0, r1) == 8);
	CHECK(fs.read("/dev/random", 8, 0, r2) == 8);
	CHECK(memcmp(r1, r2, 8) != 0);                 // stream advances
}

TEST_CASE("SynthFs /proc/uptime is nonempty text and terminates (offset EOF)") {
	SynthFs fs;
	char buf[64] = {0};
	int n = fs.read("/proc/uptime", sizeof buf, 0, buf);
	CHECK(n > 0);
	CHECK(strstr(buf, "uptime") != 0);
	CHECK(fs.read("/proc/uptime", sizeof buf, (unsigned) n, buf) == 0);  // EOF past end
}

TEST_CASE("uptimeString renders seconds and the raw tick count") {
	char b[64];
	int n = uptimeString(b, sizeof b, 2500, 1000);   // 2500 ticks @ 1000 Hz = 2 s
	CHECK(n > 0);
	CHECK(strstr(b, "2500 ticks") != 0);
	CHECK(strstr(b, "2 s") != 0);

	int z = uptimeString(b, sizeof b, 0, 1000);
	CHECK(z > 0);
	CHECK(strstr(b, "0 ticks") != 0);
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

// ---- dynamic /proc (live process table) -----------------------------------

TEST_CASE("SynthFs /proc lists a dir per live pid + the static uptime") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	const char* av[] = { "init", 0 };
	ProcTable::setCommand(a, av, 1);

	SynthFs fs;
	List<DirEntry> e;
	REQUIRE(fs.readdir("/proc", e) == 0);
	CHECK(listed(e, "uptime"));            // static gen node still there
	char pidName[16];
	snprintf(pidName, sizeof pidName, "%d", a->pid);
	CHECK(listed(e, pidName));             // dynamic per-pid directory
}

TEST_CASE("SynthFs /proc/<pid> is a dir listing comm/cmdline/stat/status") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	const char* av[] = { "nsh", 0 };
	ProcTable::setCommand(a, av, 1);
	char dir[24];
	snprintf(dir, sizeof dir, "/proc/%d", a->pid);

	SynthFs fs;
	FileStat st;
	REQUIRE(fs.stat(dir, st) == 0);
	CHECK(st.type == NODE_DIR);

	List<DirEntry> e;
	REQUIRE(fs.readdir(dir, e) == 0);
	CHECK(listed(e, "comm"));
	CHECK(listed(e, "cmdline"));
	CHECK(listed(e, "stat"));
	CHECK(listed(e, "status"));
}

TEST_CASE("SynthFs /proc/<pid>/{comm,stat,status} render the process") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);             // pid, ppid 0
	const char* av[] = { "nsh", "arg", 0 };
	ProcTable::setCommand(a, av, 2);

	SynthFs fs;
	char path[40];
	char buf[256];

	snprintf(path, sizeof path, "/proc/%d/comm", a->pid);
	memset(buf, 0, sizeof buf);
	REQUIRE(fs.read(path, sizeof buf, 0, buf) > 0);
	CHECK(strcmp(buf, "nsh\n") == 0);

	snprintf(path, sizeof path, "/proc/%d/cmdline", a->pid);
	memset(buf, 0, sizeof buf);
	REQUIRE(fs.read(path, sizeof buf, 0, buf) > 0);
	CHECK(strcmp(buf, "nsh arg\n") == 0);

	snprintf(path, sizeof path, "/proc/%d/stat", a->pid);
	memset(buf, 0, sizeof buf);
	REQUIRE(fs.read(path, sizeof buf, 0, buf) > 0);
	CHECK(strstr(buf, "(nsh)") != 0);
	CHECK(strstr(buf, " R ") != 0);               // not exited, no task -> running

	snprintf(path, sizeof path, "/proc/%d/status", a->pid);
	memset(buf, 0, sizeof buf);
	REQUIRE(fs.read(path, sizeof buf, 0, buf) > 0);
	CHECK(strstr(buf, "Name:\tnsh") != 0);
	CHECK(strstr(buf, "State:\tR") != 0);
	CHECK(strstr(buf, "Kthread:\t0") != 0);
}

TEST_CASE("SynthFs /proc: a zombie reads State Z; a kthread reads Kthread 1") {
	ProcTable::init();
	Process* z = ProcTable::alloc(0);
	const char* zv[] = { "ls", 0 };
	ProcTable::setCommand(z, zv, 1);
	z->exited = true;

	Process* k = ProcTable::alloc(0);
	const char* kv[] = { "idle", 0 };
	ProcTable::setCommand(k, kv, 1);
	k->kthread = true;

	SynthFs fs;
	char path[40], buf[256];
	snprintf(path, sizeof path, "/proc/%d/status", z->pid);
	memset(buf, 0, sizeof buf);
	fs.read(path, sizeof buf, 0, buf);
	CHECK(strstr(buf, "State:\tZ") != 0);

	snprintf(path, sizeof path, "/proc/%d/status", k->pid);
	memset(buf, 0, sizeof buf);
	fs.read(path, sizeof buf, 0, buf);
	CHECK(strstr(buf, "Kthread:\t1") != 0);
}

TEST_CASE("SynthFs /proc: absent pid and bad per-pid file error out") {
	ProcTable::init();
	ProcTable::alloc(0);                          // pid 1 exists

	SynthFs fs;
	FileStat st;
	char buf[64];
	CHECK(fs.stat("/proc/999", st) < 0);          // no such pid
	List<DirEntry> e;
	CHECK(fs.readdir("/proc/999", e) < 0);
	CHECK(fs.read("/proc/999/stat", sizeof buf, 0, buf) < 0);
	CHECK(fs.stat("/proc/1/bogus", st) < 0);      // unknown per-pid file
	CHECK(fs.read("/proc/1/bogus", sizeof buf, 0, buf) < 0);
	// /proc/uptime must still be the static node, not a (bogus) pid path.
	CHECK(fs.read("/proc/uptime", sizeof buf, 0, buf) > 0);
}
