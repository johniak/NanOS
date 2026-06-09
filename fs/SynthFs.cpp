#include "SynthFs.h"
#include "CharDevice.h"
#include "Scheduler.h"
#include "Process.h"
#include <string.h>

namespace kernel {

// Append the decimal form of v to out; returns the number of chars written.
static int utoa(unsigned v, char* out) {
	char tmp[12];
	int t = 0;
	do { tmp[t++] = (char) ('0' + v % 10); v /= 10; } while (v);
	for (int i = 0; i < t; i++)
		out[i] = tmp[t - 1 - i];
	return t;
}

int uptimeString(char* buf, int cap, unsigned ticks, unsigned hz) {
	unsigned secs = hz ? ticks / hz : 0;
	int p = 0;
	const char* a = "uptime: ";
	for (int i = 0; a[i] && p < cap - 1; i++) buf[p++] = a[i];
	p += utoa(secs, buf + p);
	const char* b = " s (";
	for (int i = 0; b[i] && p < cap - 1; i++) buf[p++] = b[i];
	p += utoa(ticks, buf + p);
	const char* c = " ticks)\n";
	for (int i = 0; c[i] && p < cap - 1; i++) buf[p++] = c[i];
	buf[p] = 0;
	return p;
}

// Render /proc/meminfo, Linux-style ("Key:<pad>value kB\n" lines). Pure for host tests.
int meminfoString(char* buf, int cap, unsigned memTotalKb, unsigned memFreeKb,
		unsigned heapTotalKb, unsigned heapFreeKb) {
	struct Row { const char* key; unsigned val; };
	Row rows[] = {
		{ "MemTotal:", memTotalKb }, { "MemFree:", memFreeKb },
		{ "MemUsed:", memTotalKb > memFreeKb ? memTotalKb - memFreeKb : 0 },
		{ "KHeapTotal:", heapTotalKb }, { "KHeapFree:", heapFreeKb },
	};
	int p = 0;
	for (unsigned r = 0; r < sizeof rows / sizeof rows[0]; r++) {
		const char* k = rows[r].key;
		int keyLen = 0;
		for (; k[keyLen] && p < cap - 1; keyLen++) buf[p++] = k[keyLen];
		// Pad to a fixed column so the values line up (Linux-style), then "<num> kB".
		const int col = 15;
		for (int s = keyLen; s < col && p < cap - 1; s++) buf[p++] = ' ';
		char num[12];
		int nlen = utoa(rows[r].val, num);
		for (int i = 0; i < nlen && p < cap - 1; i++) buf[p++] = num[i];
		const char* suf = " kB\n";
		for (int i = 0; suf[i] && p < cap - 1; i++) buf[p++] = suf[i];
	}
	buf[p] = 0;
	return p;
}

// Small append helpers for the /proc renderers below (freestanding: no snprintf).
static int putStr(char* b, int p, int cap, const char* s) {
	for (int i = 0; s[i] && p < cap - 1; i++) b[p++] = s[i];
	return p;
}
static int putUint(char* b, int p, int cap, unsigned v) {
	char num[12];
	int n = utoa(v, num);
	for (int i = 0; i < n && p < cap - 1; i++) b[p++] = num[i];
	return p;
}

// /proc/stat: the kernel activity summary. The `cpu`/`cpu0` line carries real user/system/idle
// time, converted from the kernel's tick rate to USER_HZ=100 jiffies; ctxt is the real context-
// switch count and `processes` the total forks since boot. Format matches Linux for top/htop.
int statString(char* buf, int cap, unsigned userTicks, unsigned sysTicks, unsigned idleTicks,
		unsigned hz, unsigned ctxt, unsigned forks, unsigned running, unsigned blocked) {
	unsigned div = (hz >= 100u) ? hz / 100u : 1u;   // ticks -> jiffies (USER_HZ 100)
	unsigned u = userTicks / div, s = sysTicks / div, idle = idleTicks / div;
	int p = 0;
	for (int cpu = -1; cpu < 1; cpu++) {            // "cpu" aggregate, then "cpu0"
		p = putStr(buf, p, cap, "cpu");
		if (cpu >= 0) p = putUint(buf, p, cap, (unsigned) cpu);
		else          p = putStr(buf, p, cap, " ");  // aggregate line has a double space
		// user nice system idle iowait irq softirq steal guest guest_nice
		p = putStr(buf, p, cap, " "); p = putUint(buf, p, cap, u);
		p = putStr(buf, p, cap, " 0 ");               // nice
		p = putUint(buf, p, cap, s);
		p = putStr(buf, p, cap, " "); p = putUint(buf, p, cap, idle);
		p = putStr(buf, p, cap, " 0 0 0 0 0 0\n");
	}
	p = putStr(buf, p, cap, "ctxt ");          p = putUint(buf, p, cap, ctxt);     p = putStr(buf, p, cap, "\n");
	p = putStr(buf, p, cap, "btime 0\n");
	p = putStr(buf, p, cap, "processes ");     p = putUint(buf, p, cap, forks);    p = putStr(buf, p, cap, "\n");
	p = putStr(buf, p, cap, "procs_running "); p = putUint(buf, p, cap, running);  p = putStr(buf, p, cap, "\n");
	p = putStr(buf, p, cap, "procs_blocked "); p = putUint(buf, p, cap, blocked);  p = putStr(buf, p, cap, "\n");
	buf[p] = 0;
	return p;
}

// /proc/loadavg: the 1/5/15-minute load averages (fixed-point hundredths, computed by the
// scheduler), runnable/total, and the last pid created.
int loadavgString(char* buf, int cap, unsigned load1, unsigned load5, unsigned load15,
		unsigned runnable, unsigned total, int lastPid) {
	unsigned loads[3] = { load1, load5, load15 };
	int p = 0;
	for (int i = 0; i < 3; i++) {
		p = putUint(buf, p, cap, loads[i] / 100u);       // integer part
		p = putStr(buf, p, cap, ".");
		unsigned frac = loads[i] % 100u;                 // two-digit fraction
		if (frac < 10) p = putStr(buf, p, cap, "0");
		p = putUint(buf, p, cap, frac);
		p = putStr(buf, p, cap, " ");
	}
	p = putUint(buf, p, cap, runnable);
	p = putStr(buf, p, cap, "/");
	p = putUint(buf, p, cap, total);
	p = putStr(buf, p, cap, " ");
	p = putUint(buf, p, cap, (unsigned) (lastPid < 0 ? 0 : lastPid));
	p = putStr(buf, p, cap, "\n");
	buf[p] = 0;
	return p;
}

// /proc/cpuinfo: one processor entry. Mostly static (we are a single-core i686 under QEMU).
int cpuinfoString(char* buf, int cap) {
	int p = 0;
	p = putStr(buf, p, cap,
		"processor\t: 0\n"
		"vendor_id\t: NanOS\n"
		"cpu family\t: 6\n"
		"model\t\t: 0\n"
		"model name\t: NanOS i686 (QEMU)\n"
		"cpu MHz\t\t: 0.000\n"
		"flags\t\t: fpu tsc\n"
		"\n");
	buf[p] = 0;
	return p;
}

// /proc/version: the kernel identification string.
int versionString(char* buf, int cap) {
	int p = putStr(buf, 0, cap, "NanOS version 0.1 (i686) #1 SMP\n");
	buf[p] = 0;
	return p;
}

// Length-bounded name compare (avoids strncmp, absent from the freestanding libc):
// node name `a` (NUL-terminated) equals the `blen`-char component `b`.
static bool nameEq(const char* a, const char* b, int blen) {
	int i = 0;
	for (; i < blen; i++)
		if (a[i] == 0 || a[i] != b[i])
			return false;
	return a[i] == 0;
}

SynthNode* SynthFs::mk(SynthKind kind, const char* name, unsigned perms) {
	SynthNode* n = new SynthNode();
	int i = 0;
	for (; name[i] && i < 63; i++)
		n->name[i] = name[i];
	n->name[i] = 0;
	n->kind = kind;
	n->nchild = 0;
	n->data = 0;
	n->len = 0;
	n->gen = 0;
	n->dev = 0;
	n->perms = perms;
	return n;
}

SynthNode* SynthFs::addDir(SynthNode* parent, const char* name) {
	SynthNode* n = mk(SK_DIR, name, 0555);
	if (parent->nchild < 32)
		parent->child[parent->nchild++] = n;
	return n;
}

void SynthFs::addStatic(SynthNode* parent, const char* name, const char* data, unsigned len) {
	SynthNode* n = mk(SK_STATIC, name, 0444);
	n->data = data;
	n->len = len;
	if (parent->nchild < 32)
		parent->child[parent->nchild++] = n;
}

void SynthFs::addGen(SynthNode* parent, const char* name, SynthGen g, unsigned perms) {
	SynthNode* n = mk(SK_GEN, name, perms);
	n->gen = g;
	if (parent->nchild < 32)
		parent->child[parent->nchild++] = n;
}

void SynthFs::addChar(SynthNode* parent, const char* name, CharDevice* dev, unsigned perms) {
	SynthNode* n = mk(SK_CHARDEV, name, perms);
	n->dev = dev;
	if (parent->nchild < 32)
		parent->child[parent->nchild++] = n;
}

void SynthFs::addVolume(const char* name) {
	addDir(m_disks, name);
}

// ---- generated nodes (devices + proc) -------------------------------------
// Stream devices ignore offset: null is always EOF; zero/random produce endlessly.
static int gen_null(unsigned, void*, unsigned) { return 0; }

static int gen_zero(unsigned, void* buf, unsigned n) {
	memset(buf, 0, n);
	return (int) n;
}

static int gen_random(unsigned, void* buf, unsigned n) {
	static unsigned state = 2463534242u;   // xorshift32, advances across reads
	unsigned char* p = (unsigned char*) buf;
	for (unsigned i = 0; i < n; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		p[i] = (unsigned char) state;
	}
	return (int) n;
}

// Snapshot file: render the live tick count (served by offset so `cat` terminates).
// cat reads it in a single call (n >= len), so regenerating per call is consistent.
static int gen_uptime(unsigned off, void* buf, unsigned n) {
	static char s[64];
	int len = uptimeString(s, sizeof s, kernel::Scheduler::ticks(), 1000);
	if (off >= (unsigned) len)
		return 0;
	unsigned cnt = n < (unsigned) (len - off) ? n : (unsigned) (len - off);
	memcpy(buf, s + off, cnt);
	return (int) cnt;
}

// Snapshot file: render live /proc/meminfo (served by offset so `cat` terminates).
static int gen_meminfo(unsigned off, void* buf, unsigned n) {
	static char s[256];
	int len = meminfoString(s, sizeof s, sysMemTotalKb(), sysMemFreeKb(),
			sysHeapTotalKb(), sysHeapFreeKb());
	if (off >= (unsigned) len)
		return 0;
	unsigned cnt = n < (unsigned) (len - off) ? n : (unsigned) (len - off);
	memcpy(buf, s + off, cnt);
	return (int) cnt;
}

// Serve a rendered snapshot buffer by offset (so `cat` reads it once and stops at EOF).
static int serveSnap(unsigned off, void* buf, unsigned n, const char* s, int len) {
	if (off >= (unsigned) len)
		return 0;
	unsigned cnt = n < (unsigned) (len - off) ? n : (unsigned) (len - off);
	memcpy(buf, s + off, cnt);
	return (int) cnt;
}

// Live process counts for /proc/stat and /proc/loadavg.
static void procCounts(unsigned* total, unsigned* running, unsigned* blocked) {
	ProcInfo arr[16];
	int t = ProcTable::snapshot(arr, 16);
	int r = 0, b = 0;
	for (int i = 0; i < t; i++) {
		if (arr[i].state == 'R')
			r++;
		else if (arr[i].state == 'S')
			b++;
	}
	*total = (unsigned) t;
	*running = (unsigned) r;
	*blocked = (unsigned) b;
}

static int gen_stat(unsigned off, void* buf, unsigned n) {
	static char s[512];
	unsigned total, running, blocked;
	procCounts(&total, &running, &blocked);
	unsigned u, sy, id;
	ProcTable::cpuTimes(&u, &sy, &id);
	int len = statString(s, sizeof s, u, sy, id, 1000, kernel::Scheduler::contextSwitches(),
			ProcTable::forksTotal(), running, blocked);
	return serveSnap(off, buf, n, s, len);
}

static int gen_loadavg(unsigned off, void* buf, unsigned n) {
	static char s[96];
	unsigned total, running, blocked;
	procCounts(&total, &running, &blocked);
	unsigned ld[3];
	kernel::Scheduler::loadAvg(ld);
	int len = loadavgString(s, sizeof s, ld[0], ld[1], ld[2], running, total, ProcTable::lastPid());
	return serveSnap(off, buf, n, s, len);
}

static int gen_cpuinfo(unsigned off, void* buf, unsigned n) {
	static char s[256];
	int len = cpuinfoString(s, sizeof s);
	return serveSnap(off, buf, n, s, len);
}

static int gen_version(unsigned off, void* buf, unsigned n) {
	static char s[64];
	int len = versionString(s, sizeof s);
	return serveSnap(off, buf, n, s, len);
}

SynthFs::SynthFs() {
	root = mk(SK_DIR, "/", 0555);
	m_disks = addDir(root, "disks");
	m_dev = addDir(root, "dev");
	m_proc = addDir(root, "proc");
	addDir(root, "tmp");        // marker so `ls /` shows /tmp; the tmpfs mounts over it

	addGen(m_dev, "null", gen_null, 0666);
	addGen(m_dev, "zero", gen_zero, 0666);
	addGen(m_dev, "random", gen_random, 0444);
	addGen(m_proc, "uptime", gen_uptime, 0444);
	addGen(m_proc, "meminfo", gen_meminfo, 0444);
	addGen(m_proc, "stat", gen_stat, 0444);
	addGen(m_proc, "loadavg", gen_loadavg, 0444);
	addGen(m_proc, "cpuinfo", gen_cpuinfo, 0444);
	addGen(m_proc, "version", gen_version, 0444);
}

int SynthFs::mount() { return 0; }

SynthNode* SynthFs::dirChild(SynthNode* d, const char* name, int len) {
	for (int i = 0; i < d->nchild; i++)
		if (nameEq(d->child[i]->name, name, len))
			return d->child[i];
	return 0;
}

// Walk an absolute path to its node, or 0 if missing. Empty components (leading,
// trailing, or doubled '/') are skipped, so "/", "/dev", "/dev/" all resolve.
SynthNode* SynthFs::walk(const char* path) {
	if (path[0] != '/')
		return 0;
	SynthNode* cur = root;
	int i = 1;
	while (path[i]) {
		int j = i;
		while (path[j] && path[j] != '/')
			j++;
		int len = j - i;
		if (len > 0) {
			if (cur->kind != SK_DIR)
				return 0;
			cur = dirChild(cur, path + i, len);
			if (!cur)
				return 0;
		}
		i = (path[j] == '/') ? j + 1 : j;
	}
	return cur;
}

// ---- dynamic /proc (per-process directories, Linux-style) -----------------
// Classify a path against the live process table:
//   1 = "/proc" itself (readdir appends one dir per pid), 2 = "/proc/<pid>",
//   3 = "/proc/<pid>/<file>", 0 = not a dynamic /proc path (e.g. /proc/uptime).
static int classifyProc(const char* path, int* pidOut, const char** fileOut) {
	const char* pre = "/proc";
	int i = 0;
	for (; pre[i]; i++)
		if (path[i] != pre[i])
			return 0;
	if (path[i] == 0)
		return 1;                       // "/proc"
	if (path[i] != '/')
		return 0;                       // e.g. "/procfoo"
	i++;
	if (path[i] == 0)
		return 1;                       // "/proc/"
	int pid = 0, digits = 0;
	for (; path[i] >= '0' && path[i] <= '9'; i++) {
		pid = pid * 10 + (path[i] - '0');
		digits++;
	}
	if (digits == 0)
		return 0;                       // non-numeric child (e.g. "uptime")
	*pidOut = pid;
	if (path[i] == 0)
		return 2;                       // "/proc/<pid>"
	if (path[i] != '/')
		return 0;                       // "/proc/12abc"
	i++;
	if (path[i] == 0)
		return 2;                       // "/proc/<pid>/"
	*fileOut = path + i;                // "/proc/<pid>/<file>"
	return 3;
}

static bool streq(const char* a, const char* b) {
	int i = 0;
	for (; a[i] && b[i]; i++)
		if (a[i] != b[i])
			return false;
	return a[i] == b[i];
}

static const char* const PROC_FILES[] = { "comm", "cmdline", "stat", "status", 0 };
static bool isProcFile(const char* name) {
	for (int i = 0; PROC_FILES[i]; i++)
		if (streq(name, PROC_FILES[i]))
			return true;
	return false;
}

static int appendStr(char* buf, int p, int cap, const char* s) {
	for (int i = 0; s[i] && p < cap - 1; i++)
		buf[p++] = s[i];
	return p;
}

// Render one /proc/<pid>/<file> into buf; returns its length, or -1 if unknown.
static int renderProcFile(const char* file, char* buf, int cap, const ProcInfo& pi) {
	int p = 0;
	char st[2] = { pi.state, 0 };
	if (streq(file, "comm")) {
		p = appendStr(buf, p, cap, pi.comm);
		p = appendStr(buf, p, cap, "\n");
	} else if (streq(file, "cmdline")) {
		p = appendStr(buf, p, cap, pi.cmdline);
		p = appendStr(buf, p, cap, "\n");
	} else if (streq(file, "stat")) {
		// Linux /proc/<pid>/stat fields. We fill the ones top/htop/ps read (state, ppid,
		// pgrp, session, utime, stime, num_threads, starttime); fields we do not track
		// (page-fault counters, vsize, rss, ...) are 0.
		p += utoa((unsigned) pi.pid, buf + p);                  // 1: pid
		p = appendStr(buf, p, cap, " (");
		p = appendStr(buf, p, cap, pi.comm);                    // 2: comm
		p = appendStr(buf, p, cap, ") ");
		p = appendStr(buf, p, cap, st);                         // 3: state
		p = appendStr(buf, p, cap, " ");
		p += utoa((unsigned) pi.ppid, buf + p);                 // 4: ppid
		p = appendStr(buf, p, cap, " ");
		p += utoa((unsigned) pi.pgid, buf + p);                 // 5: pgrp
		p = appendStr(buf, p, cap, " ");
		p += utoa((unsigned) pi.sid, buf + p);                  // 6: session
		p = appendStr(buf, p, cap, " 0 -1 0 0 0 0 0 ");         // 7-13: tty_nr tpgid flags faults
		p += utoa(pi.utime, buf + p);                           // 14: utime
		p = appendStr(buf, p, cap, " ");
		p += utoa(pi.stime, buf + p);                           // 15: stime
		p = appendStr(buf, p, cap, " 0 0 20 0 1 0 ");           // 16-21: cutime cstime prio nice threads itreal
		p += utoa(pi.starttime, buf + p);                       // 22: starttime
		p = appendStr(buf, p, cap, " 0 0\n");                   // 23-24: vsize rss
	} else if (streq(file, "status")) {
		p = appendStr(buf, p, cap, "Name:\t");
		p = appendStr(buf, p, cap, pi.comm);
		p = appendStr(buf, p, cap, "\nState:\t");
		p = appendStr(buf, p, cap, st);
		p = appendStr(buf, p, cap, "\nPid:\t");
		p += utoa((unsigned) pi.pid, buf + p);
		p = appendStr(buf, p, cap, "\nPPid:\t");
		p += utoa((unsigned) pi.ppid, buf + p);
		p = appendStr(buf, p, cap, "\nPgid:\t");
		p += utoa((unsigned) pi.pgid, buf + p);
		p = appendStr(buf, p, cap, "\nSid:\t");
		p += utoa((unsigned) pi.sid, buf + p);
		p = appendStr(buf, p, cap, "\nKthread:\t");
		p = appendStr(buf, p, cap, pi.kthread ? "1" : "0");
		p = appendStr(buf, p, cap, "\n");
	} else {
		return -1;
	}
	buf[p] = 0;
	return p;
}

int SynthFs::read(String path, unsigned size, unsigned off, void* buf) {
	int pid = 0;
	const char* file = 0;
	if (classifyProc((char*) path, &pid, &file) == 3) {
		ProcInfo pi;
		if (!isProcFile(file) || !ProcTable::infoByPid(pid, &pi))
			return -1;
		char tmp[320];
		int len = renderProcFile(file, tmp, sizeof tmp, pi);
		if (len < 0 || off >= (unsigned) len)
			return len < 0 ? -1 : 0;
		unsigned cnt = size < (unsigned) (len - off) ? size : (unsigned) (len - off);
		memcpy(buf, tmp + off, cnt);
		return (int) cnt;
	}
	return readNode(path, size, off, buf);
}

int SynthFs::readNode(String path, unsigned size, unsigned off, void* buf) {
	SynthNode* n = walk((char*) path);
	if (!n)
		return -1;
	if (n->kind == SK_STATIC) {
		if (off >= n->len)
			return 0;
		unsigned avail = n->len - off;
		unsigned cnt = size < avail ? size : avail;
		memcpy(buf, n->data + off, cnt);
		return (int) cnt;
	}
	if (n->kind == SK_GEN)
		return n->gen(off, buf, size);
	if (n->kind == SK_CHARDEV)
		return n->dev->read(off, buf, size);
	return -1;   // directory
}

int SynthFs::write(String path, unsigned size, unsigned off, const void* buf) {
	SynthNode* n = walk((char*) path);
	if (!n)
		return -2;             // -ENOENT
	if (n->kind != SK_CHARDEV)
		return -30;            // -EROFS: the synthetic tree is otherwise read-only
	return n->dev->write(off, buf, size);
}

int SynthFs::ioctl(String path, unsigned cmd, void* arg) {
	SynthNode* n = walk((char*) path);
	if (!n || n->kind != SK_CHARDEV)
		return -22;            // -EINVAL / -ENOTTY
	return n->dev->ioctl(cmd, arg);
}

int SynthFs::mmapInfo(String path, unsigned* physOut, unsigned* lenOut) {
	SynthNode* n = walk((char*) path);
	if (!n || n->kind != SK_CHARDEV)
		return -22;            // -EINVAL: not mmappable
	return n->dev->mmapInfo(physOut, lenOut);
}

short SynthFs::pollReady(String path, short events) {
	SynthNode* n = walk((char*) path);
	if (!n || n->kind != SK_CHARDEV)
		return events;         // non-device nodes: treat as always ready
	return n->dev->pollReady(events);
}

int SynthFs::stat(String path, FileStat& out) {
	int pid = 0;
	const char* file = 0;
	int c = classifyProc((char*) path, &pid, &file);
	if (c == 2 || c == 3) {                 // /proc/<pid> dir, or /proc/<pid>/<file>
		ProcInfo pi;
		if (!ProcTable::infoByPid(pid, &pi))
			return -1;
		if (c == 3 && !isProcFile(file))
			return -1;
		out.type = (c == 2) ? NODE_DIR : NODE_FILE;
		out.size = 0;
		out.mode = (c == 2) ? (0x4000 | 0555) : (0x8000 | 0444);
		out.nlink = 1;
		out.uid = out.gid = out.mtime = 0;
		return 0;
	}

	SynthNode* n = walk((char*) path);
	if (!n)
		return -1;
	if (n->kind == SK_DIR) {
		out.type = NODE_DIR;
		out.size = 0;
		out.mode = 0x4000 | (n->perms & 0777);
	} else {
		out.type = NODE_FILE;
		out.size = (n->kind == SK_STATIC) ? n->len : 0;
		out.mode = 0x8000 | (n->perms & 0777);
	}
	out.nlink = 1;
	out.uid = 0;
	out.gid = 0;
	out.mtime = 0;
	return 0;
}

// Append a directory entry by name + type.
static void pushEntry(List<DirEntry>& out, const char* name, NodeType type) {
	DirEntry de;
	int k = 0;
	for (; name[k] && k < 255; k++)
		de.name[k] = name[k];
	de.name[k] = 0;
	de.type = type;
	out.add(de);
}

// Every directory lists "." (itself) and ".." (parent) first, like a Unix directory —
// so `ls -la` shows them. (`ls` without -a hides dotfiles.)
static void pushDotEntries(List<DirEntry>& out) {
	pushEntry(out, ".", NODE_DIR);
	pushEntry(out, "..", NODE_DIR);
}

int SynthFs::readdir(String path, List<DirEntry>& out) {
	int pid = 0;
	const char* file = 0;
	if (classifyProc((char*) path, &pid, &file) == 2) {   // /proc/<pid> -> per-pid files
		ProcInfo pi;
		if (!ProcTable::infoByPid(pid, &pi))
			return -1;
		pushDotEntries(out);
		for (int i = 0; PROC_FILES[i]; i++) {
			DirEntry de;
			int k = 0;
			for (; PROC_FILES[i][k] && k < 255; k++)
				de.name[k] = PROC_FILES[i][k];
			de.name[k] = 0;
			de.type = NODE_FILE;
			out.add(de);
		}
		return 0;
	}

	SynthNode* n = walk((char*) path);
	if (!n || n->kind != SK_DIR)
		return -1;
	pushDotEntries(out);
	for (int i = 0; i < n->nchild; i++) {
		DirEntry de;
		int k = 0;
		for (; n->child[i]->name[k] && k < 255; k++)
			de.name[k] = n->child[i]->name[k];
		de.name[k] = 0;
		de.type = (n->child[i]->kind == SK_DIR) ? NODE_DIR : NODE_FILE;
		out.add(de);
	}
	if (n == m_proc) {                          // append one dir per live process
		ProcInfo procs[32];
		int np = ProcTable::snapshot(procs, 32);
		for (int i = 0; i < np; i++) {
			DirEntry de;
			int k = utoa((unsigned) procs[i].pid, de.name);
			de.name[k] = 0;
			de.type = NODE_DIR;
			out.add(de);
		}
	}
	return 0;
}

}
