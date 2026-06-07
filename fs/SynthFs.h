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

// A generated file: fill up to n bytes at logical offset off; return bytes produced.
typedef int (*SynthGen)(unsigned off, void* buf, unsigned n);

// Render an uptime string ("uptime: <s> s (<ticks> ticks)\n") into buf; returns its
// length. Free function so it is host-testable; used by the /proc/uptime generator.
int uptimeString(char* buf, int cap, unsigned ticks, unsigned hz);

enum SynthKind { SK_DIR, SK_STATIC, SK_GEN };

struct SynthNode {
	char name[64];
	SynthKind kind;
	SynthNode* child[32];   // SK_DIR
	int nchild;
	const char* data;       // SK_STATIC
	unsigned len;
	SynthGen gen;           // SK_GEN
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
	void addVolume(const char* name);     // marker dir under /disks

	SynthNode* dev() { return m_dev; }
	SynthNode* proc() { return m_proc; }

	// FileSystem interface
	int mount();
	int read(String path, unsigned size, unsigned off, void* buf);
	int stat(String path, FileStat& out);
	int readdir(String path, List<DirEntry>& out);
};

}
