#pragma once
#include "BlockDevice.h"
#include "String.h"
#include "List.h"
#include "Cred.h"

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
	unsigned ino;      // inode number (file identity; 0 = unknown). Hard links share it; tools
	                   // like rm/find compare it to refuse removing "." / ".." / "/".
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
	virtual int mmapInfo(String, uint64_t*, unsigned*) { return -22; }          // -EINVAL
	// Offset-aware device mmap (GEM fake offsets); default forwards offset 0 to mmapInfo.
	virtual int mmapAt(String p, uint64_t off, uint64_t* ph, unsigned* ln) {
		return off == 0 ? mmapInfo(p, ph, ln) : -22;
	}
	// poll() readiness for a path: return the ready subset of `events`. Default = ready
	// (ordinary files don't block); SynthFs forwards to the char device.
	virtual short pollReady(String, short events) { return events; }
	// The wait list a blocked reader/writer of this path parks on (a char device's queue), or
	// 0 if the path never blocks. Lets the dispatch sleep event-driven instead of tick-polling.
	virtual WaitQueue* waitQueueAt(String) { return 0; }
	// fd open/close accounting for char devices: deviceOpen returns true if `path` is a char
	// device (and bumps its open count); deviceClose drops it. Lets a pty know when its slave
	// side has no open fds left (-> master EOF). Default: not a char device / no-op.
	virtual bool deviceOpen(String) { return false; }
	virtual void deviceClose(String) {}

	// Write extensions. Default to read-only (-EROFS); a writable fs (RamFs/tmpfs)
	// overrides them. create() makes-or-truncates a regular file.
	virtual int create(String, unsigned) { return -30; }                        // -EROFS
	virtual int mknod(String, unsigned) { return -30; }                         // -EROFS (typed node; AF_UNIX S_IFSOCK)
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

	// ---- DAC policy layer (the inode_permission() analogue). The caller's credentials come
	// from a CredProvider hook; a null provider (early boot / kernel context) bypasses checks.
	typedef const Cred* (*CredProviderFn)();
	CredProviderFn credProvider;
	const Cred* caller() { return credProvider ? credProvider() : 0; }
	int statNoCheck(String path, FileStat& out);          // FS stat with NO permission check
	int maySearch(String path);                            // x on every ancestor dir; 0 / -EACCES
	int permission(String path, int want);                 // walk + want on the final object
	int mayCreate(String path);                            // walk + W on parent dir
	int mayDelete(String path);                            // walk + W on parent + sticky
	int chownNoCheck(String path, unsigned uid, unsigned gid);   // ownership set on create (no check)
	void ownNewObject(String path, bool isDir);           // stamp a new file/dir with caller identity
	static String parentOf(String path);                  // the directory holding `path`

public:
	Vfs() : credProvider(0) {}
	void setCredProvider(CredProviderFn p) { credProvider = p; }
	int checkExec(String path);   // X on the file (for execve); defined in Vfs.cpp (takes the VFS lock)
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
	// Atomic append (stat + write at EOF in ONE critical section) — two concurrent appenders to
	// the same file must never clobber each other's extension. The kernel log sinks use this.
	int append(String path, unsigned size, const void* buf);
	int ioctl(String path, unsigned cmd, void* arg);
	int mmapInfo(String path, uint64_t* physOut, unsigned* lenOut);
	int mmapAt(String path, uint64_t off, uint64_t* physOut, unsigned* lenOut);
	short pollReady(String path, short events);
	WaitQueue* waitQueueAt(String path);   // the char device's block wait list for `path`, or 0
	bool deviceOpen(String path);          // true if `path` is a char device (bumps its open count)
	void deviceClose(String path);         // drop a char device open count for `path`
	int create(String path, unsigned mode);
	int mknod(String path, unsigned mode);
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
