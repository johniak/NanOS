#include "Vfs.h"
#include "Spinlock.h"
#include <string.h>

namespace kernel {

// SMP: one coarse lock serializes the whole VFS — the mount table + path routing AND the call
// into the underlying filesystem (per-inode locking is a later optimization). Recursive because
// the public ops compose through the private DAC helpers (create->mayCreate->permission->
// maySearch->statNoCheck->resolve, all touching `mounts`), which would self-deadlock a plain lock.
// It does NOT disable interrupts: the VFS is only entered from thread (syscall) context — never an
// IRQ handler — and an op can be long (ext does polling disk I/O), so keeping IRQs on lets the BSP
// timer keep ticking. Blocking reads sleep at the syscall-dispatch layer with NO VFS op on the
// stack (pollReady/waitQueueAt each lock-and-release), so the lock is never held across a sleep.
static RecursiveSpinlock g_vfsLock;

int Vfs::checkExec(String path) {   // X on the file (for execve); defined here so it locks like the rest
	RecursiveGuard g(g_vfsLock);
	return permission(path, 1);
}

void Vfs::registerType(FileSystemType* type) {
	RecursiveGuard g(g_vfsLock);
	types.add(type);
}

int Vfs::mount(String mountpoint, String fstype, BlockDevice* dev,
		unsigned partitionLba) {
	RecursiveGuard g(g_vfsLock);
	const char* wanted = (char*) fstype;
	bool autodetect = (strcmp(wanted, "auto") == 0);
	FileSystemType* type = 0;
	for (size_t i = 0; i < types.getCount(); i++) {
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
	RecursiveGuard g(g_vfsLock);
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
	for (size_t i = 0; i < mounts.getCount(); i++) {
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

// ---- DAC policy layer -------------------------------------------------------------------

int Vfs::statNoCheck(String path, FileStat& out) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->stat(rel, out);
}

// The directory holding `path`: strip the last '/component'. Root's parent is root.
String Vfs::parentOf(String path) {
	const char* p = (char*) path;
	int len = (int) strlen(p);
	int cut = -1;
	for (int i = len - 1; i >= 0; i--)
		if (p[i] == '/') { cut = i; break; }
	if (cut <= 0) return String("/");
	char buf[256];
	if (cut >= (int) sizeof(buf)) cut = (int) sizeof(buf) - 1;
	for (int i = 0; i < cut; i++) buf[i] = p[i];
	buf[cut] = 0;
	return String(buf);
}

// Search (x) on every ANCESTOR directory of `path` (not the final component).
int Vfs::maySearch(String path) {
	const Cred* c = caller();
	if (!c) return 0;                       // no provider installed -> kernel/root context
	const char* s = (char*) path;
	char acc[256];
	int n = 0;
	acc[n++] = '/';
	for (int i = 1; s[i]; i++) {
		if (s[i] == '/') {
			acc[n] = 0;
			FileStat st;
			if (statNoCheck(String(acc), st) < 0) return -2;   // -ENOENT
			int pc = credAccess(*c, st.uid, st.gid, st.mode, 1 /*x*/, false);
			if (pc < 0) return pc;
		}
		if (n < 255) acc[n++] = s[i];
	}
	return 0;
}

int Vfs::permission(String path, int want) {
	int sc = maySearch(path);
	if (sc < 0) return sc;
	const Cred* c = caller();
	if (!c) return 0;
	FileStat st;
	if (statNoCheck(path, st) < 0) return -2;
	return credAccess(*c, st.uid, st.gid, st.mode, want, false);
}

int Vfs::mayCreate(String path) {
	int sc = maySearch(path);
	if (sc < 0) return sc;
	const Cred* c = caller();
	if (!c) return 0;
	FileStat pd;
	if (statNoCheck(parentOf(path), pd) < 0) return -2;
	return credAccess(*c, pd.uid, pd.gid, pd.mode, 2 /*w*/, false);
}

int Vfs::mayDelete(String path) {
	int mc = mayCreate(path);               // search + W on the parent directory
	if (mc < 0) return mc;
	const Cred* c = caller();
	if (!c) return 0;
	FileStat pd;
	statNoCheck(parentOf(path), pd);
	if (pd.mode & 01000) {                  // sticky parent: only owner/dir-owner/root may remove
		FileStat ts;
		if (statNoCheck(path, ts) < 0) return -2;
		return credMaySticky(*c, pd.uid, ts.uid);
	}
	return 0;
}

int Vfs::chownNoCheck(String path, unsigned uid, unsigned gid) {
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0) return -1;
	return fs->chown(rel, uid, gid);
}

// ---- public operations (gated) ----------------------------------------------------------

int Vfs::read(String path, unsigned size, unsigned off, void* buf) {
	RecursiveGuard g(g_vfsLock);
	int pc = permission(path, 4);
	if (pc < 0) return pc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->read(rel, size, off, buf);
}

int Vfs::stat(String path, FileStat& out) {
	RecursiveGuard g(g_vfsLock);
	int sc = maySearch(path);
	if (sc < 0) return sc;
	return statNoCheck(path, out);
}

int Vfs::lstat(String path, FileStat& out) {
	RecursiveGuard g(g_vfsLock);
	int sc = maySearch(path);
	if (sc < 0) return sc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->lstat(rel, out);
}

int Vfs::readlink(String path, char* buf, unsigned size) {
	RecursiveGuard g(g_vfsLock);
	int sc = maySearch(path);
	if (sc < 0) return sc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->readlink(rel, buf, size);
}

int Vfs::readdir(String path, List<DirEntry>& out) {
	// readdir reads the directory's contents: search on ancestors + R on the directory itself.
	RecursiveGuard g(g_vfsLock);
	int pc = permission(path, 4);
	if (pc < 0) return pc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->readdir(rel, out);
}

int Vfs::write(String path, unsigned size, unsigned off, const void* buf) {
	RecursiveGuard g(g_vfsLock);
	int pc = permission(path, 2);
	if (pc < 0) return pc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->write(rel, size, off, buf);
}

int Vfs::ioctl(String path, unsigned cmd, void* arg) {
	RecursiveGuard g(g_vfsLock);
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->ioctl(rel, cmd, arg);
}

short Vfs::pollReady(String path, short events) {
	RecursiveGuard g(g_vfsLock);
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return 0;
	return fs->pollReady(rel, events);
}

WaitQueue* Vfs::waitQueueAt(String path) {
	RecursiveGuard g(g_vfsLock);
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return 0;
	return fs->waitQueueAt(rel);
}

bool Vfs::deviceOpen(String path) {
	RecursiveGuard g(g_vfsLock);
	String rel;
	FileSystem* fs = resolve(path, rel);
	return fs ? fs->deviceOpen(rel) : false;
}

void Vfs::deviceClose(String path) {
	RecursiveGuard g(g_vfsLock);
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs)
		fs->deviceClose(rel);
}

int Vfs::mmapInfo(String path, uint64_t* physOut, unsigned* lenOut) {
	RecursiveGuard g(g_vfsLock);
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->mmapInfo(rel, physOut, lenOut);
}

// Give a freshly-created object the caller's identity: owner = fsuid; group = the parent
// directory's group when it is setgid, else the caller's fsgid; a setgid parent also
// propagates its S_ISGID bit onto a new sub-directory (BSD/Linux semantics).
void Vfs::ownNewObject(String path, bool isDir) {
	const Cred* c = caller();
	if (!c) return;
	FileStat pd;
	if (statNoCheck(parentOf(path), pd) < 0) return;
	unsigned g = (pd.mode & 02000) ? pd.gid : c->fsgid;
	chownNoCheck(path, c->fsuid, g);
	if (isDir && (pd.mode & 02000)) {
		FileStat ns;
		if (statNoCheck(path, ns) >= 0) {
			String rel;
			FileSystem* fs = resolve(path, rel);
			if (fs) fs->chmod(rel, ns.mode | 02000);
		}
	}
}

int Vfs::create(String path, unsigned mode) {
	RecursiveGuard g(g_vfsLock);
	FileStat ex;
	bool exists = statNoCheck(path, ex) >= 0;
	if (exists) { int pc = permission(path, 2); if (pc < 0) return pc; }   // truncate needs W on file
	else        { int mc = mayCreate(path);    if (mc < 0) return mc; }    // new file needs W on parent
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	int rc = fs->create(rel, mode);
	if (rc == 0 && !exists) ownNewObject(path, false);
	return rc;
}

int Vfs::unlink(String path) {
	RecursiveGuard g(g_vfsLock);
	int dc = mayDelete(path);
	if (dc < 0) return dc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->unlink(rel);
}

int Vfs::mkdir(String path, unsigned mode) {
	RecursiveGuard g(g_vfsLock);
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	// A mount-point root (rel == "/") always already exists; report EEXIST rather than letting
	// the fs try (and fail) to create its own root. This is what lets `mkdir -p` walk through
	// mount points such as /disks/main without aborting (POSIX: EEXIST over EROFS/ENOENT).
	const char* r = (char*) rel;
	if (r[0] == '/' && r[1] == 0)
		return -17;   // -EEXIST
	int mc = mayCreate(path);
	if (mc < 0) return mc;
	int rc = fs->mkdir(rel, mode);
	if (rc == 0) ownNewObject(path, true);
	return rc;
}

int Vfs::mknod(String path, unsigned mode) {
	RecursiveGuard g(g_vfsLock);
	int mc = mayCreate(path);
	if (mc < 0) return mc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	int rc = fs->mknod(rel, mode);
	if (rc == 0) ownNewObject(path, false);
	return rc;
}

int Vfs::rmdir(String path) {
	RecursiveGuard g(g_vfsLock);
	int dc = mayDelete(path);
	if (dc < 0) return dc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->rmdir(rel);
}

int Vfs::truncate(String path, unsigned length) {
	RecursiveGuard g(g_vfsLock);
	int pc = permission(path, 2);
	if (pc < 0) return pc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->truncate(rel, length);
}

int Vfs::rename(String oldpath, String newpath) {
	RecursiveGuard g(g_vfsLock);
	int dc = mayDelete(oldpath);          // remove from old parent (+ sticky)
	if (dc < 0) return dc;
	int cc = mayCreate(newpath);          // create in new parent
	if (cc < 0) return cc;
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
	RecursiveGuard g(g_vfsLock);
	int sc = maySearch(oldpath);          // must be able to reach the source
	if (sc < 0) return sc;
	int cc = mayCreate(newpath);          // and create the new name
	if (cc < 0) return cc;
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
	RecursiveGuard g(g_vfsLock);
	int cc = mayCreate(path);
	if (cc < 0) return cc;
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	int rc = fs->symlink(target, rel);   // target is the link's content, stored verbatim
	if (rc == 0) ownNewObject(path, false);
	return rc;
}

// chmod: must own the file (or be root). Drop S_ISGID if a non-root setter is not in the
// file's group (Linux clears setgid to prevent privilege via a group it can't claim).
int Vfs::chmod(String path, unsigned mode) {
	RecursiveGuard g(g_vfsLock);
	int sc = maySearch(path);
	if (sc < 0) return sc;
	FileStat st;
	if (statNoCheck(path, st) < 0) return -2;
	const Cred* c = caller();
	if (c) {
		int mc = credMayChmod(*c, st.uid);
		if (mc < 0) return mc;
		if (c->euid != 0 && !credInGroup(*c, st.gid)) mode &= ~02000u;   // strip setgid
	}
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->chmod(rel, mode);
}

// chown: changing owner is root-only; an owner may change group to one it belongs to. On a
// successful non-root chown, the setuid/setgid bits are cleared.
int Vfs::chown(String path, unsigned uid, unsigned gid) {
	RecursiveGuard g(g_vfsLock);
	int sc = maySearch(path);
	if (sc < 0) return sc;
	FileStat st;
	if (statNoCheck(path, st) < 0) return -2;
	const Cred* c = caller();
	if (c) {
		int mc = credMayChown(*c, st.uid, (int) uid, (int) gid);
		if (mc < 0) return mc;
	}
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	int rc = fs->chown(rel, uid, gid);
	if (rc == 0 && c && c->euid != 0 && (st.mode & 06000))
		fs->chmod(rel, st.mode & ~06000u);   // drop setuid/setgid on ownership change
	return rc;
}

int Vfs::lchown(String path, unsigned uid, unsigned gid) {
	RecursiveGuard g(g_vfsLock);
	int sc = maySearch(path);
	if (sc < 0) return sc;
	FileStat st;
	if (statNoCheck(path, st) < 0) return -2;   // lstat would do, but the owner is the same field
	const Cred* c = caller();
	if (c) {
		int mc = credMayChown(*c, st.uid, (int) uid, (int) gid);
		if (mc < 0) return mc;
	}
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->lchown(rel, uid, gid);
}

int Vfs::utimes(String path, unsigned atime, unsigned mtime) {
	RecursiveGuard g(g_vfsLock);
	int sc = maySearch(path);
	if (sc < 0) return sc;
	FileStat st;
	if (statNoCheck(path, st) < 0) return -2;
	const Cred* c = caller();
	if (c) {
		// We cannot tell utimes(NULL) ("now") from explicit times at this layer; treat as
		// explicit (the stricter owner-or-root rule). The syscall layer maps NULL->now.
		int mc = credMayUtimes(*c, st.uid, false);
		if (mc < 0) {
			// Fall back to the "now" rule: write permission also authorizes a timestamp touch.
			if (credAccess(*c, st.uid, st.gid, st.mode, 2, false) < 0) return mc;
		}
	}
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->utimes(rel, atime, mtime);
}

int Vfs::statfs(String path, StatFs& out) {
	RecursiveGuard g(g_vfsLock);
	String rel;
	FileSystem* fs = resolve(path, rel);
	if (fs == 0)
		return -1;
	return fs->statfs(rel, out);
}

}
