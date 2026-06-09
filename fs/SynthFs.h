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

namespace kernel {

struct CharDevice;   // a writable/ioctl/mmappable device node (drivers/CharDevice.h)

// A generated file: fill up to n bytes at logical offset off; return bytes produced.
typedef int (*SynthGen)(unsigned off, void* buf, unsigned n);

// Render an uptime string ("uptime: <s> s (<ticks> ticks)\n") into buf; returns its
// length. Free function so it is host-testable; used by the /proc/uptime generator.
int uptimeString(char* buf, int cap, unsigned ticks, unsigned hz);

// Render Linux-style /proc/meminfo (MemTotal/MemFree + our kernel-heap figures, all in
// kB) into buf; returns its length. Pure -> host-testable; the /proc/meminfo generator
// supplies the live numbers via the sys*Kb() accessors below.
int meminfoString(char* buf, int cap, unsigned memTotalKb, unsigned memFreeKb,
		unsigned heapTotalKb, unsigned heapFreeKb);

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
	SynthNode* child[32];   // SK_DIR
	int nchild;
	const char* data;       // SK_STATIC
	unsigned len;
	SynthGen gen;           // SK_GEN
	CharDevice* dev;        // SK_CHARDEV
	unsigned perms;         // permission bits (type bits added by stat)
};

class SynthFs: public FileSystem {
	SynthNode* root;
	SynthNode* m_disks;
	SynthNode* m_dev;
	SynthNode* m_proc;

	SynthNode* mk(SynthKind kind, const char* name, unsigned perms);
	SynthNode* dirChild(SynthNode* d, const char* name, int len);
	SynthNode* walk(const char* path);

public:
	SynthFs();

	SynthNode* addDir(SynthNode* parent, const char* name);
	void addStatic(SynthNode* parent, const char* name, const char* data, unsigned len);
	void addGen(SynthNode* parent, const char* name, SynthGen g, unsigned perms);
	void addChar(SynthNode* parent, const char* name, CharDevice* dev, unsigned perms);
	void addVolume(const char* name);     // marker dir under /disks

	SynthNode* dev() { return m_dev; }
	SynthNode* proc() { return m_proc; }

	// FileSystem interface
	int mount();
	int read(String path, unsigned size, unsigned off, void* buf);
	int stat(String path, FileStat& out);
	int readdir(String path, List<DirEntry>& out);
	// Device extensions (override the FileSystem defaults; only char-device nodes honor them).
	int write(String path, unsigned size, unsigned off, const void* buf);
	int ioctl(String path, unsigned cmd, void* arg);
	int mmapInfo(String path, unsigned* physOut, unsigned* lenOut);
	short pollReady(String path, short events);

private:
	int readNode(String path, unsigned size, unsigned off, void* buf);   // static-tree read
};

}
