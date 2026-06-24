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

TEST_CASE("SynthFs mkdir: existing path is EEXIST, missing path is EROFS") {
	SynthFs fs;
	// The synthetic tree is read-only, but an existing node must report EEXIST (POSIX orders
	// existence before writability) so `mkdir -p` skips it instead of aborting.
	CHECK(fs.mkdir("/disks", 0755) == -17);   // -EEXIST (exists)
	CHECK(fs.mkdir("/dev", 0755) == -17);     // -EEXIST (exists)
	CHECK(fs.mkdir("/nope", 0755) == -30);    // -EROFS (does not exist, cannot create)
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

TEST_CASE("SynthFs /proc/uptime is the Linux numeric format and terminates (offset EOF)") {
	SynthFs fs;
	char buf[64] = {0};
	int n = fs.read("/proc/uptime", sizeof buf, 0, buf);
	CHECK(n > 0);
	// "<uptime>.<cc> <idle>.<cc>\n" — a digit, a dot, a space separating the two floats.
	CHECK((buf[0] >= '0' && buf[0] <= '9'));
	CHECK(strchr(buf, '.') != 0);
	CHECK(strchr(buf, ' ') != 0);
	CHECK(strstr(buf, "uptime") == 0);   // NOT the old human string
	CHECK(fs.read("/proc/uptime", sizeof buf, (unsigned) n, buf) == 0);  // EOF past end
}

TEST_CASE("uptimeString renders the Linux /proc/uptime format (uptime idle, hundredths)") {
	char b[64];
	// 2530 ticks @1000Hz = 2.53 s uptime; 1200 idle ticks = 1.20 s idle.
	int n = uptimeString(b, sizeof b, 2530, 1200, 1000);
	CHECK(n > 0);
	CHECK(strcmp(b, "2.53 1.20\n") == 0);    // two floats, space-separated, hundredths, newline

	// Sub-10 hundredths must be zero-padded ("0.05", not "0.5").
	int z = uptimeString(b, sizeof b, 50, 0, 1000);
	CHECK(z > 0);
	CHECK(strcmp(b, "0.05 0.00\n") == 0);
}

TEST_CASE("SynthFs /proc/meminfo is readable Linux-style text and terminates") {
	SynthFs fs;
	char buf[512] = {0};
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

TEST_CASE("meminfoString emits the keys htop reads (MemAvailable/Buffers/Cached/Swap)") {
	char b[512];
	int n = meminfoString(b, sizeof b, 4096, 1024, 256, 128);
	CHECK(n > 0);
	CHECK(strstr(b, "MemTotal:") != 0);
	CHECK(strstr(b, "MemFree:") != 0);
	CHECK(strstr(b, "MemAvailable:") != 0);   // htop: available memory (== free here)
	CHECK(strstr(b, "Buffers:") != 0);
	CHECK(strstr(b, "Cached:") != 0);
	CHECK(strstr(b, "SwapTotal:") != 0);
	CHECK(strstr(b, "SwapFree:") != 0);
}

TEST_CASE("statmString: size==resident==memKb/4 pages, rest zero") {
	char b[64];
	int n = statmString(b, sizeof b, 4096 /*KiB*/);   // 1024 pages
	CHECK(n > 0);
	CHECK(strcmp(b, "1024 1024 0 0 0 0 0\n") == 0);
}

TEST_CASE("SynthFs exposes /proc/<pid>/task/<pid> thread dir (htop main-thread scan)") {
	ProcTable::init();
	Process* p = ProcTable::alloc(0);
	const char* av[] = { "demo", 0 };
	ProcTable::setCommand(p, av, 1);
	SynthFs fs;
	char base[32], tbase[48];
	snprintf(base, sizeof base, "/proc/%d", p->pid);
	snprintf(tbase, sizeof tbase, "/proc/%d/task/%d", p->pid, p->pid);

	// /proc/<pid> readdir includes "task"
	List<DirEntry> e;
	REQUIRE(fs.readdir(String(base), e) >= 0);
	CHECK(listed(e, "task"));

	// /proc/<pid>/task is a directory listing the main tid
	FileStat st;
	char taskdir[40]; snprintf(taskdir, sizeof taskdir, "/proc/%d/task", p->pid);
	REQUIRE(fs.stat(String(taskdir), st) >= 0);
	CHECK(st.type == NODE_DIR);
	List<DirEntry> te;
	REQUIRE(fs.readdir(String(taskdir), te) >= 0);
	char tidname[12]; int kk = 0; { int v=p->pid; char tmp[12]; int n=0; if(v==0)tmp[n++]='0'; while(v){tmp[n++]='0'+v%10;v/=10;} while(n)tidname[kk++]=tmp[--n]; tidname[kk]=0; }
	CHECK(listed(te, tidname));

	// /proc/<pid>/task/<pid>/stat reads the same content as /proc/<pid>/stat
	char tstat[64], pstat[48];
	snprintf(tstat, sizeof tstat, "%s/stat", tbase);
	snprintf(pstat, sizeof pstat, "%s/stat", base);
	char b1[256] = {0}, b2[256] = {0};
	int n1 = fs.read(String(tstat), sizeof b1, 0, b1);
	int n2 = fs.read(String(pstat), sizeof b2, 0, b2);
	CHECK(n1 > 0);
	CHECK(n1 == n2);
	CHECK(strcmp(b1, b2) == 0);
}

TEST_CASE("SynthFs serves /proc/<pid>/statm for a live process") {
	ProcTable::init();
	Process* p = ProcTable::alloc(0);
	const char* av[] = { "demo", 0 };
	ProcTable::setCommand(p, av, 1);
	p->brkBase = 0x800000;
	p->brkCur = 0x800000 + 4096 * 1024;   // 4096 KiB -> 1024 pages

	char path[32];
	snprintf(path, sizeof path, "/proc/%d/statm", p->pid);
	SynthFs fs;
	char buf[64] = {0};
	int n = fs.read(path, sizeof buf, 0, buf);
	REQUIRE(n > 0);
	CHECK(strcmp(buf, "1024 1024 0 0 0 0 0\n") == 0);

	// statm appears in the per-pid directory listing.
	List<DirEntry> e;
	char dir[32];
	snprintf(dir, sizeof dir, "/proc/%d", p->pid);
	REQUIRE(fs.readdir(dir, e) >= 0);
	CHECK(listed(e, "statm"));
}

TEST_CASE("statusString includes Uid, Gid, VmSize, VmRSS, Threads for htop") {
	ProcInfo pi;
	memset(&pi, 0, sizeof pi);
	pi.pid = 7; pi.ppid = 1; pi.pgid = 7; pi.sid = 7;
	pi.state = 'R'; pi.memKb = 2048; pi.nthreads = 3;
	strcpy(pi.comm, "demo");
	char b[512];
	int n = statusString(b, sizeof b, pi);
	CHECK(n > 0);
	CHECK(strstr(b, "Name:\tdemo") != 0);
	CHECK(strstr(b, "Uid:\t0\t0\t0\t0") != 0);
	CHECK(strstr(b, "Gid:\t0\t0\t0\t0") != 0);
	CHECK(strstr(b, "VmSize:\t2048 kB") != 0);
	CHECK(strstr(b, "VmRSS:\t2048 kB") != 0);
	CHECK(strstr(b, "Threads:\t3") != 0);
}

TEST_CASE("SynthFs /proc/<pid>/stat carries real vsize/rss and num_threads") {
	ProcTable::init();
	Process* p = ProcTable::alloc(0);
	const char* av[] = { "demo", 0 };
	ProcTable::setCommand(p, av, 1);
	p->brkBase = 0x800000;
	p->brkCur = 0x800000 + 8192 * 1024;   // 8192 KiB -> vsize 8388608 bytes, rss 2048 pages
	p->threadCount = 4;

	char path[32];
	snprintf(path, sizeof path, "/proc/%d/stat", p->pid);
	SynthFs fs;
	char buf[384] = {0};
	int n = fs.read(path, sizeof buf, 0, buf);
	REQUIRE(n > 0);
	CHECK(strstr(buf, " 8388608 ") != 0);   // field 23: vsize (bytes)
	CHECK(strstr(buf, " 4 ") != 0);          // field 20: num_threads
	// htop's stat parser reads through field 39 (processor); the line must carry >=39
	// space-separated fields or htop drops the process. Count the spaces.
	int spaces = 0;
	for (const char* c = buf; *c && *c != '\n'; c++) if (*c == ' ') spaces++;
	CHECK(spaces >= 38);                      // 39 fields => >=38 separators
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
	int mmapInfo(uint64_t* p, unsigned* l) { *p = 0xABC000; *l = 0x1000; return 0; }
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
	uint64_t p = 0; unsigned l = 0;
	CHECK(fs.mmapInfo("/dev/fb0", &p, &l) == 0);
	CHECK(p == 0xABC000u);
	CHECK(l == 0x1000u);

	// /dev/fb0 shows up in the /dev listing.
	List<DirEntry> e;
	REQUIRE(fs.readdir("/dev", e) == 0);
	CHECK(listed(e, "fb0"));

	// A plain read-only generated node (no write sink): write is -EROFS, ioctl/mmap are -EINVAL.
	CHECK(fs.write("/proc/uptime", 4, 0, buf) == -30);
	CHECK(fs.ioctl("/dev/zero", 0, buf) < 0);
	CHECK(fs.mmapInfo("/dev/zero", &p, &l) < 0);

	// The stream devices have a write sink: writing to them SUCCEEDS (accepts all bytes), the way
	// /dev/null is a bit bucket and writing to /dev/[u]random mixes into the entropy pool. Without
	// this, a write returns -EROFS and a stdio flush to one of them spins (e.g. Dropbear hangs).
	CHECK(fs.write("/dev/null", 4, 0, buf) == 4);
	CHECK(fs.write("/dev/zero", 4, 0, buf) == 4);
	CHECK(fs.write("/dev/random", 4, 0, buf) == 4);
	CHECK(fs.write("/dev/urandom", 4, 0, buf) == 4);
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
	unsigned u[1] = { 1000 }, s[1] = { 500 }, id[1] = { 2500 };
	int n = statString(b, sizeof b, 1, u, s, id, 1000, 9999, 42, 1, 3, 1781000000u);
	CHECK(n > 0);
	CHECK(strstr(b, "cpu  100 0 50 250 ") != 0);         // aggregate: user nice system idle
	CHECK(strstr(b, "cpu0 100 0 50 250 ") != 0);
	CHECK(strstr(b, "\nctxt 9999\n") != 0);
	CHECK(strstr(b, "\nbtime 1781000000\n") != 0);       // real boot epoch, not a fixed 0
	CHECK(strstr(b, "\nprocesses 42\n") != 0);           // total forks since boot
	CHECK(strstr(b, "\nprocs_running 1\n") != 0);
	CHECK(strstr(b, "\nprocs_blocked 3\n") != 0);
}

TEST_CASE("statString renders one cpuN line per core; the aggregate is their sum") {
	char b[1024];
	// 4 cores with distinct busy levels; @1000Hz -> /10 jiffies.
	unsigned u[4] = { 1000, 2000, 0, 500 };
	unsigned s[4] = { 500, 0, 1000, 500 };
	unsigned id[4] = { 2500, 8000, 4000, 9000 };
	int n = statString(b, sizeof b, 4, u, s, id, 1000, 7, 0, 0, 0, 0);
	CHECK(n > 0);
	// aggregate cpu = sum: user (1000+2000+0+500)/10=350, sys (500+0+1000+500)/10=200, idle (23500)/10=2350
	CHECK(strstr(b, "cpu  350 0 200 2350 ") != 0);
	CHECK(strstr(b, "\ncpu0 100 0 50 250 ") != 0);
	CHECK(strstr(b, "\ncpu1 200 0 0 800 ") != 0);
	CHECK(strstr(b, "\ncpu2 0 0 100 400 ") != 0);
	CHECK(strstr(b, "\ncpu3 50 0 50 900 ") != 0);
	CHECK(strstr(b, "\ncpu4 ") == 0);                    // exactly 4 cores, no cpu4
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
	ci.khz = 2500500;                                  // 2500.500 MHz
	strcpy(ci.flags, "fpu tsc sse sse2");
	char b[1024];
	CHECK(cpuinfoString(b, sizeof b, 1, ci) > 0);
	CHECK(strstr(b, "vendor_id\t: GenuineIntel") != 0);
	CHECK(strstr(b, "model name\t: Test CPU @ 2.50GHz") != 0);
	CHECK(strstr(b, "cpu family\t: 6") != 0);
	CHECK(strstr(b, "model\t\t: 42") != 0);
	CHECK(strstr(b, "cpu MHz\t\t: 2500.500") != 0);    // measured clock, 3-digit fraction
	CHECK(strstr(b, "flags\t\t: fpu tsc sse sse2") != 0);
	CHECK(strstr(b, "processor\t: 0") != 0);
	// 4 cores -> processor entries 0..3 (this is how nproc/htop count cores).
	CHECK(cpuinfoString(b, sizeof b, 4, ci) > 0);
	CHECK(strstr(b, "processor\t: 0") != 0);
	CHECK(strstr(b, "processor\t: 3") != 0);
	CHECK(strstr(b, "processor\t: 4") == 0);
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

TEST_CASE("SynthFs /sys/devices/system/cpu lists cpu0..cpuN-1 after populateSysCpu") {
	ProcTable::init();
	SynthFs fs;
	// Before population, the cpu dir is empty (only . and ..).
	List<DirEntry> before;
	CHECK(fs.readdir("/sys/devices/system/cpu", before) == 0);

	fs.populateSysCpu(4);
	List<DirEntry> after;
	CHECK(fs.readdir("/sys/devices/system/cpu", after) == 0);
	bool cpu0 = false, cpu3 = false, cpu4 = false, online = false;
	for (int i = 0; i < after.getCount(); i++) {
		if (strcmp(after[i].name, "cpu0") == 0) cpu0 = true;
		if (strcmp(after[i].name, "cpu3") == 0) cpu3 = true;
		if (strcmp(after[i].name, "cpu4") == 0) cpu4 = true;
		if (strcmp(after[i].name, "online") == 0) online = true;
	}
	CHECK(cpu0);
	CHECK(cpu3);
	CHECK(!cpu4);          // exactly 4 cores -> cpu0..cpu3
	CHECK(online);         // the cpu-level online/present/possible range files

	// Each cpuN/online reads "1\n"; the cpu-level online range reads "0-3\n".
	char b[16];
	int n = fs.read("/sys/devices/system/cpu/cpu2/online", sizeof b, 0, b);
	CHECK(n == 2);
	CHECK(b[0] == '1');
	n = fs.read("/sys/devices/system/cpu/online", sizeof b, 0, b);
	b[n] = 0;
	CHECK(strcmp(b, "0-3\n") == 0);
}

TEST_CASE("SynthFs populateSysCpu with a single CPU yields cpu0 and range \"0\"") {
	ProcTable::init();
	SynthFs fs;
	fs.populateSysCpu(1);
	char b[16];
	int n = fs.read("/sys/devices/system/cpu/online", sizeof b, 0, b);
	b[n] = 0;
	CHECK(strcmp(b, "0\n") == 0);
	List<DirEntry> d;
	CHECK(fs.readdir("/sys/devices/system/cpu/cpu0", d) == 0);   // cpu0 exists and is a dir
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
