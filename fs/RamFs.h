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

struct RamNode;
// A directory entry: a NAME bound to a node. Names live on the entry (not the node) so a hard
// link can give the same node a second name in another (or the same) directory.
struct RamDirent {
	char name[64];
	RamNode* node;
};

struct RamNode {
	bool isDir;
	bool isSymlink;            // a symbolic link: `link` holds the target, no data/children
	char* link;                // symlink target (malloc'd), or 0
	RamDirent* ent;            // directory entries (malloc/realloc'd, grows on demand)
	int nchild;                // number of entries
	int childCap;              // allocated slots in ent[]
	unsigned char* data;       // file contents (malloc/realloc'd)
	unsigned size;             // bytes of valid data
	unsigned cap;              // allocated capacity
	unsigned mode;             // permission bits (0..0777) as passed to create/mkdir
	unsigned uid, gid;         // owner / group (chown)
	int nlink;                 // hard-link count (a node can sit in several directories)
	unsigned atime, mtime;     // access / modification times (utimes)
};

class RamFs: public FileSystem {
	RamNode* root;

	RamNode* mk(bool isDir);
	bool addChild(RamNode* d, const char* name, int len, RamNode* c);   // bind name -> node
	RamNode* dirChild(RamNode* d, const char* name, int len);
	RamNode* walk(const char* path);
	RamNode* walkFollow(const char* path, int depth);   // resolves a final-component symlink
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
	int rmdir(String path);
	int rename(String oldpath, String newpath);
	int link(String oldpath, String newpath);
	int symlink(String target, String path);
	int truncate(String path, unsigned length);
	int chmod(String path, unsigned mode);
	int chown(String path, unsigned uid, unsigned gid);
	int utimes(String path, unsigned atime, unsigned mtime);
	int statfs(String path, StatFs& out);
	int lstat(String path, FileStat& out);
	int readlink(String path, char* buf, unsigned size);
};

// FileSystemType so the VFS can mount "tmpfs" (no block device needed).
class RamFsType: public FileSystemType {
public:
	const char* name() { return "tmpfs"; }
	bool probe(BlockDevice*, unsigned) { return false; }   // never auto-probed
	FileSystem* create(BlockDevice*, unsigned) { return new RamFs(); }
};

}
