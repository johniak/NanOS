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

public:
	void registerType(FileSystemType* type);
	int mount(String mountpoint, String fstype, BlockDevice* dev, unsigned partitionLba);
	int read(String path, unsigned size, unsigned off, void* buf);
	int stat(String path, FileStat& out);
	int readdir(String path, List<DirEntry>& out);
};

}
