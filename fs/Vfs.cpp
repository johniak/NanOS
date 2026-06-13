#include "Vfs.h"
#include <string.h>

namespace kernel {

void Vfs::registerType(FileSystemType* type) {
	types.add(type);
}

int Vfs::mount(String mountpoint, String fstype, BlockDevice* dev,
		unsigned partitionLba) {
	const char* wanted = (char*) fstype;
	bool autodetect = (strcmp(wanted, "auto") == 0);
	FileSystemType* type = 0;
	for (int i = 0; i < types.getCount(); i++) {
		if (autodetect) {
			if (types[i]->probe(dev, partitionLba)) {
				type = types[i];
				break;
			}
		} else if (strcmp(types[i]->name(), wanted) == 0) {
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

	return addMount(mountpoint, fs);
}

int Vfs::mount(String mountpoint, FileSystem* fs) {
	if (fs == 0)
		return -1;
	int rc = fs->mount();
	if (rc < 0)
		return rc;
	return addMount(mountpoint, fs);
}

int Vfs::addMount(String mountpoint, FileSystem* fs) {
	Mount m;
	const char* mp = (char*) mountpoint;
	const int cap = (int) sizeof(m.mountpoint);
	int len = 0;
	while (mp[len])
		len++;
	if (len >= cap)
		return -36;            // -ENAMETOOLONG: reject, don't truncate (would misroute paths)
	for (int n = 0; n < len; n++)
		m.mountpoint[n] = mp[n];
	m.mountpoint[len] = 0;
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

int Vfs::lstat(String path, FileStat& out) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->lstat(rel, out);
}

int Vfs::readlink(String path, char* buf, unsigned size) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->readlink(rel, buf, size);
}

int Vfs::readdir(String path, List<DirEntry>& out) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->readdir(rel, out);
}

int Vfs::write(String path, unsigned size, unsigned off, const void* buf) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->write(rel, size, off, buf);
}

int Vfs::ioctl(String path, unsigned cmd, void* arg) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->ioctl(rel, cmd, arg);
}

short Vfs::pollReady(String path, short events) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return 0;
	return fs->pollReady(rel, events);
}

WaitQueue* Vfs::waitQueueAt(String path) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return 0;
	return fs->waitQueueAt(rel);
}

bool Vfs::deviceOpen(String path) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	return fs ? fs->deviceOpen(rel) : false;
}

void Vfs::deviceClose(String path) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs)
		fs->deviceClose(rel);
}

int Vfs::mmapInfo(String path, unsigned* physOut, unsigned* lenOut) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->mmapInfo(rel, physOut, lenOut);
}

int Vfs::create(String path, unsigned mode) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->create(rel, mode);
}

int Vfs::unlink(String path) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->unlink(rel);
}

int Vfs::mkdir(String path, unsigned mode) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->mkdir(rel, mode);
}

int Vfs::mknod(String path, unsigned mode) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->mknod(rel, mode);
}

int Vfs::rmdir(String path) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->rmdir(rel);
}

int Vfs::truncate(String path, unsigned length) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->truncate(rel, length);
}

int Vfs::rename(String oldpath, String newpath) {
	String relOld, relNew;
	FileSystem* fo = resolve(oldpath, relOld);
	FileSystem* fn = resolve(newpath, relNew);
	if (fo == 0 || fn == 0)
		return -1;
	if (fo != fn)
		return -18;                 // -EXDEV: rename cannot cross filesystems
	return fo->rename(relOld, relNew);
}

int Vfs::link(String oldpath, String newpath) {
	String relOld, relNew;
	FileSystem* fo = resolve(oldpath, relOld);
	FileSystem* fn = resolve(newpath, relNew);
	if (fo == 0 || fn == 0)
		return -1;
	if (fo != fn)
		return -18;                 // -EXDEV: hard links cannot cross filesystems
	return fo->link(relOld, relNew);
}

int Vfs::symlink(String target, String path) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->symlink(target, rel);   // target is the link's content, stored verbatim
}

int Vfs::chmod(String path, unsigned mode) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->chmod(rel, mode);
}

int Vfs::chown(String path, unsigned uid, unsigned gid) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->chown(rel, uid, gid);
}

int Vfs::lchown(String path, unsigned uid, unsigned gid) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->lchown(rel, uid, gid);
}

int Vfs::utimes(String path, unsigned atime, unsigned mtime) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->utimes(rel, atime, mtime);
}

int Vfs::statfs(String path, StatFs& out) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->statfs(rel, out);
}

}
