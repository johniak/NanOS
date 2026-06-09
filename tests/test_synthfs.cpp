#include "doctest.h"
#include "SynthFs.h"
#include "CharDevice.h"
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
	fs.addStatic(fs.proc(), "selftest", "NanOS v0\n", 9);

	FileStat st;
	REQUIRE(fs.stat("/proc/selftest", st) == 0);
	CHECK(st.type == NODE_FILE);
	CHECK(st.size == 9);

	char buf[16] = {0};
	CHECK(fs.read("/proc/selftest", 9, 0, buf) == 9);
	CHECK(strncmp(buf, "NanOS v0\n", 9) == 0);

	char p[8] = {0};
	CHECK(fs.read("/proc/selftest", 3, 6, p) == 3);   // bytes [6,9) = "v0\n"
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

TEST_CASE("SynthFs grows a directory past the old 32-child cap — nothing dropped") {
	SynthFs fs;
	const int N = 80;                        // well past the former fixed child[32]
	for (int i = 0; i < N; i++) {
		char name[16];
		snprintf(name, sizeof name, "v%d", i);
		fs.addVolume(name);                  // each adds a child under /disks
	}
	List<DirEntry> e;
	REQUIRE(fs.readdir("/disks", e) == 0);
	for (int i = 0; i < N; i++) {
		char name[16];
		snprintf(name, sizeof name, "v%d", i);
		CHECK(listed(e, name));              // every volume must be visible
	}
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

TEST_CASE("SynthFs /proc/meminfo is readable Linux-style text and terminates") {
	SynthFs fs;
	char buf[256] = {0};
	int n = fs.read("/proc/meminfo", sizeof buf, 0, buf);
	CHECK(n > 0);
	CHECK(strstr(buf, "MemTotal:") != 0);
	CHECK(strstr(buf, "MemFree:") != 0);
	CHECK(strstr(buf, "kB") != 0);
	CHECK(fs.read("/proc/meminfo", sizeof buf, (unsigned) n, buf) == 0);   // EOF past end
}

TEST_CASE("meminfoString renders the kB rows and computes MemUsed") {
	char b[256];
	int n = meminfoString(b, sizeof b, 131072, 120000, 5120, 5000);
	CHECK(n > 0);
	CHECK(strstr(b, "MemTotal:") != 0);
	CHECK(strstr(b, "131072 kB") != 0);
	CHECK(strstr(b, "120000 kB") != 0);
	CHECK(strstr(b, "MemUsed:") != 0);
	CHECK(strstr(b, "11072 kB") != 0);          // 131072 - 120000
	CHECK(strstr(b, "KHeapFree:") != 0);
	CHECK(strstr(b, "5000 kB") != 0);
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

// ---- char devices (SK_CHARDEV: /dev/fb0-style) -----------------------------

namespace {
struct FakeDev : CharDevice {
	int lastCmd = 0;
	int read(unsigned, void* b, unsigned n) { memset(b, 0x7E, n); return (int) n; }
	int write(unsigned, const void*, unsigned n) { return (int) n; }
	int ioctl(unsigned cmd, void*) { lastCmd = (int) cmd; return 0; }
	int mmapInfo(unsigned* p, unsigned* l) { *p = 0xABC000; *l = 0x1000; return 0; }
};
}

TEST_CASE("SynthFs SK_CHARDEV routes read/write/ioctl/mmapInfo to the device") {
	SynthFs fs;
	static FakeDev dev;                       // static: the fs keeps the pointer
	fs.addChar(fs.dev(), "fb0", &dev, 0666);

	char buf[4] = { 0 };
	CHECK(fs.read("/dev/fb0", 4, 0, buf) == 4);
	CHECK((unsigned char) buf[0] == 0x7E);
	CHECK(fs.write("/dev/fb0", 4, 0, buf) == 4);
	CHECK(fs.ioctl("/dev/fb0", 0x4600, buf) == 0);
	CHECK(dev.lastCmd == 0x4600);
	unsigned p = 0, l = 0;
	CHECK(fs.mmapInfo("/dev/fb0", &p, &l) == 0);
	CHECK(p == 0xABC000u);
	CHECK(l == 0x1000u);

	// /dev/fb0 shows up in the /dev listing.
	List<DirEntry> e;
	REQUIRE(fs.readdir("/dev", e) == 0);
	CHECK(listed(e, "fb0"));

	// A non-device node: write is -EROFS, ioctl/mmap are -EINVAL.
	CHECK(fs.write("/dev/zero", 4, 0, buf) == -30);
	CHECK(fs.ioctl("/dev/zero", 0, buf) < 0);
	CHECK(fs.mmapInfo("/dev/zero", &p, &l) < 0);
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

// ---- Stage 5: Linux-format /proc renderers ------------------------------------------

TEST_CASE("statString renders real user/system/idle jiffies + ctxt/processes") {
	char b[512];
	// user=1000 sys=500 idle=2500 ticks @1000Hz -> /10 = 100/50/250 jiffies.
	int n = statString(b, sizeof b, 1000, 500, 2500, 1000, 9999, 42, 1, 3, 1781000000u);
	CHECK(n > 0);
	CHECK(strstr(b, "cpu  100 0 50 250 ") != 0);         // aggregate: user nice system idle
	CHECK(strstr(b, "cpu0 100 0 50 250 ") != 0);
	CHECK(strstr(b, "\nctxt 9999\n") != 0);
	CHECK(strstr(b, "\nbtime 1781000000\n") != 0);       // real boot epoch, not a fixed 0
	CHECK(strstr(b, "\nprocesses 42\n") != 0);           // total forks since boot
	CHECK(strstr(b, "\nprocs_running 1\n") != 0);
	CHECK(strstr(b, "\nprocs_blocked 3\n") != 0);
}

TEST_CASE("loadavgString renders fixed-point loads + runnable/total + last pid") {
	char b[96];
	int n = loadavgString(b, sizeof b, 150, 75, 0, 2, 5, 7);   // 1.50 0.75 0.00
	CHECK(n > 0);
	CHECK(strcmp(b, "1.50 0.75 0.00 2/5 7\n") == 0);
}

TEST_CASE("cpuinfoString renders real CPUID fields; versionString the kernel string") {
	arch::CpuInfo ci;
	strcpy(ci.vendor, "GenuineIntel");
	strcpy(ci.brand, "Test CPU @ 2.50GHz");
	ci.family = 6; ci.model = 42; ci.stepping = 7;
	strcpy(ci.flags, "fpu tsc sse sse2");
	char b[384];
	CHECK(cpuinfoString(b, sizeof b, ci) > 0);
	CHECK(strstr(b, "vendor_id\t: GenuineIntel") != 0);
	CHECK(strstr(b, "model name\t: Test CPU @ 2.50GHz") != 0);
	CHECK(strstr(b, "cpu family\t: 6") != 0);
	CHECK(strstr(b, "model\t\t: 42") != 0);
	CHECK(strstr(b, "flags\t\t: fpu tsc sse sse2") != 0);
	char v[64];
	CHECK(versionString(v, sizeof v) > 0);
	CHECK(strstr(v, "NanOS version") != 0);
}

TEST_CASE("SynthFs /proc/{stat,loadavg,cpuinfo,version} are readable and terminate") {
	ProcTable::init();
	ProcTable::alloc(0);
	SynthFs fs;
	const char* files[] = { "/proc/stat", "/proc/loadavg", "/proc/cpuinfo", "/proc/version" };
	for (int i = 0; i < 4; i++) {
		char buf[512];
		int n = fs.read(files[i], sizeof buf, 0, buf);
		CHECK(n > 0);
		CHECK(fs.read(files[i], sizeof buf, (unsigned) n, buf) == 0);   // EOF past end
	}
}

TEST_CASE("SynthFs /proc/<pid>/stat carries pgrp + session; status has Pgid/Sid") {
	ProcTable::init();
	Process* sh = ProcTable::alloc(0);
	const char* av[] = { "nsh", 0 };
	ProcTable::setCommand(sh, av, 1);
	Process* c = ProcTable::alloc(sh->pid);   // child in its own group/session
	ProcTable::setCommand(c, av, 1);
	c->pgid = c->pid; c->sid = sh->sid;

	SynthFs fs;
	char path[40], buf[256];
	snprintf(path, sizeof path, "/proc/%d/stat", c->pid);
	memset(buf, 0, sizeof buf);
	REQUIRE(fs.read(path, sizeof buf, 0, buf) > 0);
	// "<pid> (nsh) R <ppid> <pgrp> <sid> ..."
	char want[48];
	snprintf(want, sizeof want, "(nsh) R %d %d %d ", sh->pid, c->pid, sh->sid);
	CHECK(strstr(buf, want) != 0);

	snprintf(path, sizeof path, "/proc/%d/status", c->pid);
	memset(buf, 0, sizeof buf);
	REQUIRE(fs.read(path, sizeof buf, 0, buf) > 0);
	char pg[24];
	snprintf(pg, sizeof pg, "Pgid:\t%d", c->pid);
	CHECK(strstr(buf, pg) != 0);
	snprintf(pg, sizeof pg, "Sid:\t%d", sh->sid);
	CHECK(strstr(buf, pg) != 0);
}
