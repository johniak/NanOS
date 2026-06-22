/*
 * SynthFs.h — an in-memory synthetic filesystem.
 *
 * Backs the virtual root "/" of NanOS: a tree of nodes (directories, static files,
 * and generated "device"/proc files) walked by path. Physical disks are NOT here —
 * they mount under /disks/<name> as separate Vfs mounts; SynthFs only holds the
 * namespace skeleton (/, /disks, /dev, /proc) and marker entries for volumes.
 *
 * Implements the path-based FileSystem interface, so it plugs into Vfs unchanged.
 * Pure software -> host-testable.
 */
#pragma once
#include "Vfs.h"
#include <arch/cpu.h>   // arch::CpuInfo for the /proc/cpuinfo renderer

namespace kernel {

struct CharDevice;   // a writable/ioctl/mmappable device node (drivers/CharDevice.h)

// A generated file: fill up to n bytes at logical offset off; return bytes produced.
typedef int (*SynthGen)(unsigned off, void* buf, unsigned n);

// Optional write sink for a generated file: consume n bytes at offset off; return bytes
// accepted (or a negative errno). Lets a SK_GEN node be writable (e.g. /dev/random and
// /dev/urandom, where a write mixes the supplied bytes into the entropy pool, Linux-style).
typedef int (*SynthWrite)(unsigned off, const void* buf, unsigned n);

// Render an uptime string ("uptime: <s> s (<ticks> ticks)\n") into buf; returns its
// length. Free function so it is host-testable; used by the /proc/uptime generator.
int uptimeString(char* buf, int cap, unsigned ticks, unsigned hz);

// Render Linux-style /proc/meminfo (MemTotal/MemFree + our kernel-heap figures, all in
// kB) into buf; returns its length. Pure -> host-testable; the /proc/meminfo generator
// supplies the live numbers via the sys*Kb() accessors below.
int meminfoString(char* buf, int cap, unsigned memTotalKb, unsigned memFreeKb,
		unsigned heapTotalKb, unsigned heapFreeKb);

// More Linux-format /proc renderers (pure -> host-testable). The /proc generators feed them
// live data (ticks, process counts) via the sys*/Scheduler accessors.
//   /proc/stat    — the `cpu` jiffies line + ctxt/btime/processes/procs_running.
//   /proc/loadavg — 1/5/15-min load (we report runnable as a coarse 0.NN), runnable/total, last pid.
//   /proc/cpuinfo — one processor entry (model + flags).
//   /proc/version — kernel identification string.
int statString(char* buf, int cap, unsigned userTicks, unsigned sysTicks, unsigned idleTicks,
		unsigned hz, unsigned ctxt, unsigned forks, unsigned running, unsigned blocked,
		unsigned btime);
int loadavgString(char* buf, int cap, unsigned load1, unsigned load5, unsigned load15,
		unsigned runnable, unsigned total, int lastPid);
int cpuinfoString(char* buf, int cap, const arch::CpuInfo& ci);
int versionString(char* buf, int cap);

// Render Linux-style /proc/<pid>/statm ("size resident shared text lib data dt", in 4 KiB
// pages) into buf; returns its length. NanOS has no page/text/shared split, so size==resident
// (all resident — no swap) == memKb/4 and the rest is 0. Pure -> host-testable.
int statmString(char* buf, int cap, unsigned memKb);

// Live system memory figures in kB. Implemented in the kernel (Kernel.cpp) over the
// frame allocator + byte heap + boot memory map; stubbed in the host test harness.
unsigned sysMemTotalKb();
unsigned sysMemFreeKb();
unsigned sysHeapTotalKb();
unsigned sysHeapFreeKb();

enum SynthKind { SK_DIR, SK_STATIC, SK_GEN, SK_CHARDEV };

struct SynthNode {
	char name[64];
	SynthKind kind;
	SynthNode** child;      // SK_DIR children (grows on demand)
	int nchild;
	int childCap;           // allocated slots in child[]
	const char* data;       // SK_STATIC
	unsigned len;
	SynthGen gen;           // SK_GEN
	SynthWrite genWrite;    // SK_GEN: optional write sink (0 -> writes return -EROFS)
	CharDevice* dev;        // SK_CHARDEV
	unsigned perms;         // permission bits (type bits added by stat)
};

class SynthFs: public FileSystem {
	SynthNode* root;
	SynthNode* m_disks;
	SynthNode* m_dev;
	SynthNode* m_proc;

	SynthNode* mk(SynthKind kind, const char* name, unsigned perms);
	void addChild(SynthNode* parent, SynthNode* n);   // append, growing child[] as needed
	SynthNode* dirChild(SynthNode* d, const char* name, int len);
	SynthNode* walk(const char* path);

public:
	SynthFs();

	SynthNode* addDir(SynthNode* parent, const char* name);
	void addStatic(SynthNode* parent, const char* name, const char* data, unsigned len);
	SynthNode* addGen(SynthNode* parent, const char* name, SynthGen g, unsigned perms);
	void addChar(SynthNode* parent, const char* name, CharDevice* dev, unsigned perms);
	void addVolume(const char* name);     // marker dir under /disks

	SynthNode* dev() { return m_dev; }
	SynthNode* proc() { return m_proc; }

	// FileSystem interface
	int mount();
	int read(String path, unsigned size, unsigned off, void* buf);
	int stat(String path, FileStat& out);
	int readdir(String path, List<DirEntry>& out);
	// mkdir on the read-only synthetic tree always fails, but POSIX orders existence before
	// writability: an existing path must report EEXIST, not EROFS. Without this, `mkdir -p`
	// (and cp -r, which mkdir's each component) aborts on the first existing read-only parent
	// like /disks when given an absolute path under /disks/main.
	int mkdir(String path, unsigned mode);
	// Device extensions (override the FileSystem defaults; only char-device nodes honor them).
	int write(String path, unsigned size, unsigned off, const void* buf);
	int ioctl(String path, unsigned cmd, void* arg);
	int mmapInfo(String path, uint64_t* physOut, unsigned* lenOut);
	short pollReady(String path, short events);
	WaitQueue* waitQueueAt(String path);
	bool deviceOpen(String path);
	void deviceClose(String path);

private:
	int readNode(String path, unsigned size, unsigned off, void* buf);   // static-tree read
};

}
