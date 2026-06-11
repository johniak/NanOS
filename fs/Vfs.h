#pragma once
#include "BlockDevice.h"
#include "String.h"
#include "List.h"

namespace kernel {

struct WaitQueue;   // event wait list (kernel/WaitQueue.h); a char device may expose one

enum NodeType { NODE_FILE, NODE_DIR, NODE_OTHER, NODE_SYMLINK };

struct FileStat {
	NodeType type;
	unsigned size;
	unsigned mode;     // ext i_mode: format bits (S_IF*) | permission bits
	unsigned nlink;    // hard-link count
	unsigned uid;      // owner id
	unsigned gid;      // group id
	unsigned mtime;    // last-modification time (epoch seconds)
};

// Filesystem-wide statistics (statfs/fstatfs).
struct StatFs {
	unsigned blockSize;     // f_bsize
	unsigned totalBlocks;   // f_blocks
	unsigned freeBlocks;    // f_bfree
	unsigned totalInodes;   // f_files
	unsigned freeInodes;    // f_ffree
	unsigned nameMax;       // f_namelen
};

// Names are bounded (ext2 caps at 255) and stored inline so DirEntry/Mount stay
// trivially copyable — safe to hold in List<T> without String copy semantics.
struct DirEntry {
	char name[256];
	NodeType type;
};

// A mounted filesystem instance bound to a BlockDevice.
class FileSystem {
public:
	virtual ~FileSystem() {}
	virtual int mount() = 0;                                              // 0 ok, <0 err
	virtual int read(String path, unsigned size, unsigned off, void* buf) = 0; // bytes, <0 err
	virtual int stat(String path, FileStat& out) = 0;                    // 0 ok, <0 err
	virtual int readdir(String path, List<DirEntry>& out) = 0;           // 0 ok, <0 err

	// Symbolic-link support. lstat() stats the link itself (no follow); the default is
	// stat() since a filesystem with no symlinks can't tell them apart. readlink() reads a
	// link's target; default -EINVAL (-22, "not a symlink") for filesystems without links.
	virtual int lstat(String path, FileStat& out) { return stat(path, out); }
	virtual int readlink(String, char*, unsigned) { return -22; }       // -EINVAL

	// Device extensions. Default to "unsupported" so ordinary read-only filesystems
	// (ext2/ext4) need not implement them; SynthFs overrides them for char devices.
	virtual int write(String, unsigned, unsigned, const void*) { return -30; }  // -EROFS
	virtual int ioctl(String, unsigned, void*) { return -22; }                  // -EINVAL
	virtual int mmapInfo(String, unsigned*, unsigned*) { return -22; }          // -EINVAL
	// poll() readiness for a path: return the ready subset of `events`. Default = ready
	// (ordinary files don't block); SynthFs forwards to the char device.
	virtual short pollReady(String, short events) { return events; }
	// The wait list a blocked reader/writer of this path parks on (a char device's queue), or
	// 0 if the path never blocks. Lets the dispatch sleep event-driven instead of tick-polling.
	virtual WaitQueue* waitQueueAt(String) { return 0; }

	// Write extensions. Default to read-only (-EROFS); a writable fs (RamFs/tmpfs)
	// overrides them. create() makes-or-truncates a regular file.
	virtual int create(String, unsigned) { return -30; }                        // -EROFS
	virtual int unlink(String) { return -30; }                                  // -EROFS
	virtual int mkdir(String, unsigned) { return -30; }                         // -EROFS
	// Namespace + size mutations (Phase 3). Defaults read-only; ext + RamFs override.
	virtual int rmdir(String) { return -30; }                                   // -EROFS
	virtual int rename(String, String) { return -30; }                          // -EROFS
	virtual int link(String, String) { return -30; }                           // -EROFS
	virtual int symlink(String, String) { return -30; }                        // -EROFS (target, path)
	virtual int truncate(String, unsigned) { return -30; }                     // -EROFS
	// Metadata mutations + filesystem stats (Phase 4). Defaults read-only / unsupported.
	virtual int chmod(String, unsigned) { return -30; }                        // -EROFS
	virtual int chown(String, unsigned, unsigned) { return -30; }              // -EROFS (uid, gid)
	virtual int lchown(String p, unsigned uid, unsigned gid) { return chown(p, uid, gid); }  // no-follow; default follows
	virtual int utimes(String, unsigned, unsigned) { return -30; }             // -EROFS (atime, mtime)
	virtual int statfs(String, StatFs&) { return -22; }                        // -EINVAL
};

// Factory registered by type name; creates a FileSystem for a device.
// This is the kext-extensibility hook: a loaded module registers its type here.
class FileSystemType {
public:
	virtual ~FileSystemType() {}
	virtual const char* name() = 0;                                      // e.g. "ext2"
	// Inspect the superblock and report whether this driver handles the fs.
	// Used by mount("auto", ...) to pick a driver at runtime.
	virtual bool probe(BlockDevice* dev, unsigned partitionLba) = 0;
	virtual FileSystem* create(BlockDevice* dev, unsigned partitionLba) = 0;
};

class Vfs {
	struct Mount {
		char mountpoint[256];
		FileSystem* fs;
	};
	List<FileSystemType*> types;
	List<Mount> mounts;

	// Longest mountpoint that prefixes path; fills relative path (always starts
	// with '/'). Returns 0 if no mount matches.
	FileSystem* resolve(String path, String& relative);

	// Record a mountpoint -> filesystem binding. Returns 0, or -ENAMETOOLONG if the
	// mountpoint does not fit (rejected rather than silently truncated -> wrong routing).
	int addMount(String mountpoint, FileSystem* fs);

public:
	void registerType(FileSystemType* type);
	int mount(String mountpoint, String fstype, BlockDevice* dev, unsigned partitionLba);
	// Mount an already-built filesystem (e.g. the synthetic root, which has no
	// BlockDevice). Calls fs->mount() then records the binding.
	int mount(String mountpoint, FileSystem* fs);
	int read(String path, unsigned size, unsigned off, void* buf);
	int stat(String path, FileStat& out);
	int lstat(String path, FileStat& out);
	int readlink(String path, char* buf, unsigned size);
	int readdir(String path, List<DirEntry>& out);
	int write(String path, unsigned size, unsigned off, const void* buf);
	int ioctl(String path, unsigned cmd, void* arg);
	int mmapInfo(String path, unsigned* physOut, unsigned* lenOut);
	short pollReady(String path, short events);
	WaitQueue* waitQueueAt(String path);   // the char device's block wait list for `path`, or 0
	int create(String path, unsigned mode);
	int unlink(String path);
	int mkdir(String path, unsigned mode);
	int rmdir(String path);
	int rename(String oldpath, String newpath);   // -EXDEV if the paths cross mountpoints
	int link(String oldpath, String newpath);      // hard link, same mount only
	int symlink(String target, String path);       // `target` is stored verbatim as link content
	int truncate(String path, unsigned length);
	int chmod(String path, unsigned mode);
	int chown(String path, unsigned uid, unsigned gid);
	int lchown(String path, unsigned uid, unsigned gid);
	int utimes(String path, unsigned atime, unsigned mtime);
	int statfs(String path, StatFs& out);
};

}
