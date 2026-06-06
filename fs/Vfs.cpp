#include "Vfs.h"
#include <string.h>

namespace kernel {

void Vfs::registerType(FileSystemType* type) {
	types.add(type);
}

int Vfs::mount(String mountpoint, String fstype, BlockDevice* dev,
		unsigned partitionLba) {
	const char* wanted = (char*) fstype;
	FileSystemType* type = 0;
	for (int i = 0; i < types.getCount(); i++) {
		if (strcmp(types[i]->name(), wanted) == 0) {
			type = types[i];
			break;
		}
	}
	if (type == 0)
		return -1;

	FileSystem* fs = type->create(dev, partitionLba);
	if (fs == 0)
		return -1;
	int rc = fs->mount();
	if (rc < 0)
		return rc;

	Mount m;
	const char* mp = (char*) mountpoint;
	int n = 0;
	while (mp[n] && n < 63) {
		m.mountpoint[n] = mp[n];
		n++;
	}
	m.mountpoint[n] = 0;
	m.fs = fs;
	mounts.add(m);
	return 0;
}

// mp is a prefix of path respecting path separators: the char after the match
// must be end-of-string or '/'. "/" is special and matches any absolute path.
static bool prefixMatches(const char* path, const char* mp) {
	if (mp[0] == '/' && mp[1] == 0)
		return path[0] == '/';
	int i = 0;
	while (mp[i]) {
		if (path[i] != mp[i])
			return false;
		i++;
	}
	return path[i] == 0 || path[i] == '/';
}

FileSystem* Vfs::resolve(String path, String& relative) {
	const char* p = (char*) path;
	FileSystem* best = 0;
	int bestLen = -1;
	const char* bestMp = 0;
	for (int i = 0; i < mounts.getCount(); i++) {
		const char* mp = mounts[i].mountpoint;
		int ml = (int) strlen(mp);
		if (prefixMatches(p, mp) && ml > bestLen) {
			bestLen = ml;
			best = mounts[i].fs;
			bestMp = mp;
		}
	}
	if (best == 0)
		return 0;

	if (bestMp[0] == '/' && bestMp[1] == 0) {
		relative = path;                 // root mount: relative == full path
	} else {
		const char* rest = p + bestLen;  // boundary guarantees rest is "" or "/..."
		if (rest[0] == 0)
			relative = "/";
		else
			relative = String(rest);
	}
	return best;
}

int Vfs::read(String path, unsigned size, unsigned off, void* buf) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->read(rel, size, off, buf);
}

int Vfs::stat(String path, FileStat& out) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->stat(rel, out);
}

int Vfs::readdir(String path, List<DirEntry>& out) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->readdir(rel, out);
}

}
