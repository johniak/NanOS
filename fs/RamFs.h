/*
 * RamFs.h — an in-memory writable filesystem (a tmpfs, like Linux tmpfs / BSD mfs).
 *
 * Unlike SynthFs (the read-only synthetic namespace) and ext2/ext4 (read-only disks),
 * RamFs supports create/write/unlink/mkdir: a tree of directories and files whose
 * contents live in malloc'd buffers. NanOS mounts it at /tmp so programs that must
 * write (Doom's config + savegames) have a real, POSIX-shaped place to do it. Contents
 * vanish on reboot — exactly tmpfs semantics. Pure software -> host-testable.
 */
#pragma once
#include "Vfs.h"

namespace kernel {

struct RamNode {
	char name[64];
	bool isDir;
	RamNode** child;           // directory children (malloc/realloc'd, grows on demand)
	int nchild;                // number of children
	int childCap;              // allocated slots in child[]
	unsigned char* data;       // file contents (malloc/realloc'd)
	unsigned size;             // bytes of valid data
	unsigned cap;              // allocated capacity
	unsigned mtime;
};

class RamFs: public FileSystem {
	RamNode* root;

	RamNode* mk(const char* name, int len, bool isDir);
	bool addChild(RamNode* d, RamNode* c);   // append a child, growing child[] as needed
	RamNode* dirChild(RamNode* d, const char* name, int len);
	RamNode* walk(const char* path);
	// Resolve everything but the last component: returns the parent directory and sets
	// leaf/leafLen to the final name (0 if the path is malformed or a parent is missing).
	RamNode* walkParent(const char* path, const char*& leaf, int& leafLen);
	bool ensureCap(RamNode* f, unsigned want);

public:
	RamFs();

	// FileSystem interface.
	int mount();
	int read(String path, unsigned size, unsigned off, void* buf);
	int stat(String path, FileStat& out);
	int readdir(String path, List<DirEntry>& out);
	int write(String path, unsigned size, unsigned off, const void* buf);
	int create(String path, unsigned mode);   // make-or-truncate a regular file
	int unlink(String path);
	int mkdir(String path, unsigned mode);
};

// FileSystemType so the VFS can mount "tmpfs" (no block device needed).
class RamFsType: public FileSystemType {
public:
	const char* name() { return "tmpfs"; }
	bool probe(BlockDevice*, unsigned) { return false; }   // never auto-probed
	FileSystem* create(BlockDevice*, unsigned) { return new RamFs(); }
};

}
