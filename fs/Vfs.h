#pragma once
#include "BlockDevice.h"
#include "String.h"
#include "List.h"

namespace kernel {

enum NodeType { NODE_FILE, NODE_DIR, NODE_OTHER };

struct FileStat {
	NodeType type;
	unsigned size;
	unsigned mode;     // ext i_mode: format bits (S_IF*) | permission bits
	unsigned nlink;    // hard-link count
	unsigned uid;      // owner id
	unsigned gid;      // group id
	unsigned mtime;    // last-modification time (epoch seconds)
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

	// Device extensions. Default to "unsupported" so ordinary read-only filesystems
	// (ext2/ext4) need not implement them; SynthFs overrides them for char devices.
	virtual int write(String, unsigned, unsigned, const void*) { return -30; }  // -EROFS
	virtual int ioctl(String, unsigned, void*) { return -22; }                  // -EINVAL
	virtual int mmapInfo(String, unsigned*, unsigned*) { return -22; }          // -EINVAL
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
		char mountpoint[64];
		FileSystem* fs;
	};
	List<FileSystemType*> types;
	List<Mount> mounts;

	// Longest mountpoint that prefixes path; fills relative path (always starts
	// with '/'). Returns 0 if no mount matches.
	FileSystem* resolve(String path, String& relative);

	// Record a mountpoint -> filesystem binding.
	void addMount(String mountpoint, FileSystem* fs);

public:
	void registerType(FileSystemType* type);
	int mount(String mountpoint, String fstype, BlockDevice* dev, unsigned partitionLba);
	// Mount an already-built filesystem (e.g. the synthetic root, which has no
	// BlockDevice). Calls fs->mount() then records the binding.
	int mount(String mountpoint, FileSystem* fs);
	int read(String path, unsigned size, unsigned off, void* buf);
	int stat(String path, FileStat& out);
	int readdir(String path, List<DirEntry>& out);
	int write(String path, unsigned size, unsigned off, const void* buf);
	int ioctl(String path, unsigned cmd, void* arg);
	int mmapInfo(String path, unsigned* physOut, unsigned* lenOut);
};

}
