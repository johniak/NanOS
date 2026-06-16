/*
 * RamFs.cpp — the in-memory writable filesystem (tmpfs). See RamFs.h.
 *
 * A small tree of RamNodes. Directories hold RamDirent entries (name -> node), so a node can
 * appear under several names (hard links); files hold a malloc'd, geometrically-grown data
 * buffer. Errors use the kernel's negative-errno convention, matching the rest of the VFS.
 */
#include "RamFs.h"
#include "string.h"

namespace kernel {

namespace {
const int E_NOENT = -2;
const int E_NOTDIR = -20;
const int E_ISDIR = -21;
const int E_INVAL = -22;
const int E_EXIST = -17;
const int E_NOSPC = -28;
const int E_NOTEMPTY = -39;

bool nameEq(const char* a, const char* b, int len) {
	for (int i = 0; i < len; i++)
		if (a[i] != b[i] || a[i] == 0)
			return false;
	return a[len] == 0;
}
}

RamFs::RamFs() {
	root = mk(true);
}

int RamFs::mount() { return 0; }

RamNode* RamFs::mk(bool isDir) {
	RamNode* n = (RamNode*) malloc(sizeof(RamNode));
	memset(n, 0, sizeof(RamNode));
	n->isDir = isDir;
	n->mode = isDir ? 0755 : 0644;   // sensible default; create/mkdir override from the arg
	n->nlink = 1;
	return n;
}

// Bind `name` to node `c` in directory `d`, geometrically growing the entry array.
bool RamFs::addChild(RamNode* d, const char* name, int len, RamNode* c) {
	if (d->nchild >= d->childCap) {
		int cap = d->childCap ? d->childCap * 2 : 8;
		RamDirent* p = (RamDirent*) realloc(d->ent, (unsigned) cap * sizeof(RamDirent));
		if (!p)
			return false;
		d->ent = p;
		d->childCap = cap;
	}
	RamDirent* e = &d->ent[d->nchild++];
	if (len > 63) len = 63;
	for (int i = 0; i < len; i++) e->name[i] = name[i];
	e->name[len] = 0;
	e->node = c;
	return true;
}

RamNode* RamFs::dirChild(RamNode* d, const char* name, int len) {
	if (!d->isDir)
		return 0;
	for (int i = 0; i < d->nchild; i++)
		if (nameEq(d->ent[i].name, name, len))
			return d->ent[i].node;
	return 0;
}

// Walk an absolute (mount-relative) path to its node, or 0 if missing. Empty components
// are skipped so "/", "/a", "/a/" all resolve.
RamNode* RamFs::walk(const char* path) {
	if (!path || path[0] != '/')
		return 0;
	RamNode* cur = root;
	int i = 1;
	while (path[i]) {
		int j = i;
		while (path[j] && path[j] != '/')
			j++;
		int len = j - i;
		if (len > 0) {
			cur = dirChild(cur, path + i, len);
			if (!cur)
				return 0;
		}
		i = (path[j] == '/') ? j + 1 : j;
	}
	return cur;
}

// Resolve a path, following a symbolic link in the FINAL component (depth-guarded). Intermediate
// symlink components are not followed (rare in a tmpfs); absolute and parent-relative targets work.
RamNode* RamFs::walkFollow(const char* path, int depth) {
	if (depth > 8)
		return 0;
	RamNode* n = walk(path);
	if (!n || !n->isSymlink || !n->link)
		return n;
	const char* tgt = n->link;
	if (tgt[0] == '/')
		return walkFollow(tgt, depth + 1);
	char buf[512];
	int end = (int) strlen(path);
	while (end > 0 && path[end - 1] != '/') end--;       // keep the trailing '/'
	int k = 0;
	for (int i = 0; i < end && k < 400; i++) buf[k++] = path[i];
	for (int i = 0; tgt[i] && k < 510; i++) buf[k++] = tgt[i];
	buf[k] = 0;
	return walkFollow(buf, depth + 1);
}

// Resolve the parent directory of the last component. Sets leaf/leafLen to the final
// name. Returns 0 if the path is malformed, the parent is missing, or it is not a dir.
RamNode* RamFs::walkParent(const char* path, const char*& leaf, int& leafLen) {
	if (!path || path[0] != '/')
		return 0;
	int end = (int) strlen(path);
	while (end > 0 && path[end - 1] == '/')
		end--;                       // ignore trailing slashes
	int start = end;
	while (start > 0 && path[start - 1] != '/')
		start--;
	leaf = path + start;
	leafLen = end - start;
	if (leafLen <= 0 || leafLen > 63)
		return 0;
	RamNode* cur = root;
	int i = 1;
	while (i < start) {
		int j = i;
		while (j < start && path[j] != '/')
			j++;
		int len = j - i;
		if (len > 0) {
			cur = dirChild(cur, path + i, len);
			if (!cur || !cur->isDir)
				return 0;
		}
		i = j + 1;
	}
	return cur;
}

bool RamFs::ensureCap(RamNode* f, unsigned want) {
	if (want <= f->cap)
		return true;
	unsigned cap = f->cap ? f->cap : 64;
	while (cap < want)
		cap *= 2;
	unsigned char* p = (unsigned char*) realloc(f->data, cap);
	if (!p)
		return false;
	f->data = p;
	f->cap = cap;
	return true;
}

int RamFs::read(String path, unsigned size, unsigned off, void* buf) {
	RamNode* n = walkFollow((char*) path, 0);
	if (!n)
		return E_NOENT;
	if (n->isDir)
		return E_ISDIR;
	if (off >= n->size)
		return 0;
	unsigned avail = n->size - off;
	unsigned k = size < avail ? size : avail;
	memcpy(buf, n->data + off, k);
	return (int) k;
}

int RamFs::write(String path, unsigned size, unsigned off, const void* buf) {
	RamNode* n = walkFollow((char*) path, 0);
	if (!n)
		return E_NOENT;
	if (n->isDir)
		return E_ISDIR;
	if (!ensureCap(n, off + size))
		return E_NOSPC;
	if (off > n->size)                       // writing past EOF zero-fills the gap
		memset(n->data + n->size, 0, off - n->size);
	memcpy(n->data + off, buf, size);
	if (off + size > n->size)
		n->size = off + size;
	return (int) size;
}

int RamFs::create(String path, unsigned mode) {
	const char* leaf;
	int len;
	RamNode* parent = walkParent((char*) path, leaf, len);
	if (!parent)
		return E_NOENT;
	RamNode* existing = dirChild(parent, leaf, len);
	if (existing) {
		if (existing->isDir)
			return E_ISDIR;
		existing->size = 0;                  // make-or-truncate
		return 0;
	}
	RamNode* node = mk(false);
	if (!node || !addChild(parent, leaf, len, node)) {
		if (node) free(node);
		return E_NOSPC;
	}
	node->mode = mode & 0777;
	return 0;
}

// Create a typed node (used for AF_UNIX S_IFSOCK bind names). Unlike create(), an existing path is
// EEXIST (not make-or-truncate) — a bound socket name must be unlinked before it can be rebound,
// exactly like Linux. The format bits of `mode` select the type (0xC000 => socket).
int RamFs::mknod(String path, unsigned mode) {
	const char* leaf;
	int len;
	RamNode* parent = walkParent((char*) path, leaf, len);
	if (!parent)
		return E_NOENT;
	if (dirChild(parent, leaf, len))
		return E_EXIST;
	RamNode* node = mk(false);
	if (!node || !addChild(parent, leaf, len, node)) {
		if (node) free(node);
		return E_NOSPC;
	}
	node->isSocket = ((mode & 0xF000u) == 0xC000u);
	node->mode = mode & 0777;
	return 0;
}

int RamFs::mkdir(String path, unsigned mode) {
	const char* leaf;
	int len;
	RamNode* parent = walkParent((char*) path, leaf, len);
	if (!parent)
		return E_NOENT;
	if (dirChild(parent, leaf, len))
		return E_EXIST;
	RamNode* node = mk(true);
	if (!node || !addChild(parent, leaf, len, node)) {
		if (node) free(node);
		return E_NOSPC;
	}
	node->mode = mode & 0777;
	return 0;
}

// Free a node when its last name goes away.
static void releaseNode(RamNode* c) {
	if (--c->nlink <= 0) {
		if (c->data) free(c->data);
		if (c->link) free(c->link);
		if (c->ent) free(c->ent);
		free(c);
	}
}

int RamFs::unlink(String path) {
	const char* leaf;
	int len;
	RamNode* parent = walkParent((char*) path, leaf, len);
	if (!parent)
		return E_NOENT;
	for (int i = 0; i < parent->nchild; i++) {
		if (nameEq(parent->ent[i].name, leaf, len)) {
			RamNode* c = parent->ent[i].node;
			if (c->isDir)
				return E_ISDIR;              // unlink targets files; rmdir is separate
			for (int j = i; j < parent->nchild - 1; j++)
				parent->ent[j] = parent->ent[j + 1];
			parent->nchild--;
			releaseNode(c);
			return 0;
		}
	}
	return E_NOENT;
}

// Fill a FileStat from a node (shared by stat/lstat).
static void fillStat(FileStat& out, RamNode* n) {
	out.type = n->isSymlink ? NODE_SYMLINK : n->isDir ? NODE_DIR : n->isSocket ? NODE_OTHER : NODE_FILE;
	out.size = n->isSymlink ? (n->link ? (unsigned) strlen(n->link) : 0) : (n->isDir || n->isSocket ? 0 : n->size);
	unsigned fmt = n->isSymlink ? 0xA000u : n->isDir ? 0x4000u : n->isSocket ? 0xC000u : 0x8000u;
	out.mode = fmt | (n->mode & 0777);
	out.nlink = (unsigned) (n->nlink < 1 ? 1 : n->nlink);
	out.uid = n->uid;
	out.gid = n->gid;
	out.mtime = n->mtime;
	out.ino = (unsigned) (unsigned long) n;   // per-node identity (distinct from disk inodes)
}

int RamFs::stat(String path, FileStat& out) {
	RamNode* n = walkFollow((char*) path, 0);   // stat follows symlinks
	if (!n)
		return E_NOENT;
	fillStat(out, n);
	return 0;
}

int RamFs::lstat(String path, FileStat& out) {
	RamNode* n = walk((char*) path);            // lstat stats the link itself
	if (!n)
		return E_NOENT;
	fillStat(out, n);
	return 0;
}

int RamFs::readlink(String path, char* buf, unsigned size) {
	RamNode* n = walk((char*) path);
	if (!n)
		return E_NOENT;
	if (!n->isSymlink || !n->link)
		return E_INVAL;
	unsigned k = 0;
	while (n->link[k] && k < size) { buf[k] = n->link[k]; k++; }
	return (int) k;
}

int RamFs::readdir(String path, List<DirEntry>& out) {
	RamNode* n = walk((char*) path);
	if (!n)
		return E_NOENT;
	if (!n->isDir)
		return E_NOTDIR;
	DirEntry dot;
	dot.type = NODE_DIR;
	dot.name[0] = '.'; dot.name[1] = 0;
	out.add(dot);
	dot.name[1] = '.'; dot.name[2] = 0;
	out.add(dot);
	for (int i = 0; i < n->nchild; i++) {
		DirEntry e;
		int k = 0;
		for (; n->ent[i].name[k] && k < 255; k++)
			e.name[k] = n->ent[i].name[k];
		e.name[k] = 0;
		RamNode* c = n->ent[i].node;
		e.type = c->isSymlink ? NODE_SYMLINK : c->isDir ? NODE_DIR : NODE_FILE;
		out.add(e);
	}
	return 0;
}

// Detach entry `idx` from directory `d` (compacting), returning the node; not freed.
static RamNode* detach(RamNode* d, int idx) {
	RamNode* c = d->ent[idx].node;
	for (int j = idx; j < d->nchild - 1; j++)
		d->ent[j] = d->ent[j + 1];
	d->nchild--;
	return c;
}

int RamFs::rmdir(String path) {
	const char* leaf; int len;
	RamNode* parent = walkParent((char*) path, leaf, len);
	if (!parent)
		return E_NOENT;
	for (int i = 0; i < parent->nchild; i++)
		if (nameEq(parent->ent[i].name, leaf, len)) {
			RamNode* c = parent->ent[i].node;
			if (!c->isDir)
				return E_NOTDIR;
			if (c->nchild != 0)
				return E_NOTEMPTY;
			detach(parent, i);
			if (c->ent) free(c->ent);
			free(c);
			return 0;
		}
	return E_NOENT;
}

int RamFs::rename(String oldpath, String newpath) {
	const char* ol; int oln;
	RamNode* op = walkParent((char*) oldpath, ol, oln);
	if (!op) return E_NOENT;
	int oidx = -1;
	for (int i = 0; i < op->nchild; i++)
		if (nameEq(op->ent[i].name, ol, oln)) { oidx = i; break; }
	if (oidx < 0) return E_NOENT;
	const char* nl; int nln;
	RamNode* np = walkParent((char*) newpath, nl, nln);
	if (!np) return E_NOENT;
	// Replace an existing destination (empty dir / file).
	for (int i = 0; i < np->nchild; i++)
		if (nameEq(np->ent[i].name, nl, nln)) {
			RamNode* d = np->ent[i].node;
			if (d->isDir && d->nchild != 0) return E_NOTEMPTY;
			detach(np, i);
			if (d->isDir) { if (d->ent) free(d->ent); free(d); }
			else releaseNode(d);
			if (np == op && i < oidx) oidx--;     // detaching shifted the source index
			break;
		}
	RamNode* node = detach(op, oidx);
	if (!addChild(np, nl, nln, node)) return E_NOSPC;
	return 0;
}

int RamFs::link(String oldpath, String newpath) {
	RamNode* t = walk((char*) oldpath);
	if (!t) return E_NOENT;
	if (t->isDir) return -1;                 // -EPERM: no hard links to directories
	const char* nl; int nln;
	RamNode* np = walkParent((char*) newpath, nl, nln);
	if (!np) return E_NOENT;
	if (dirChild(np, nl, nln)) return E_EXIST;
	if (!addChild(np, nl, nln, t)) return E_NOSPC;   // the SAME node, a second name
	t->nlink++;
	return 0;
}

int RamFs::symlink(String target, String path) {
	const char* leaf; int len;
	RamNode* parent = walkParent((char*) path, leaf, len);
	if (!parent) return E_NOENT;
	if (dirChild(parent, leaf, len)) return E_EXIST;
	RamNode* node = mk(false);
	if (!node) return E_NOSPC;
	node->isSymlink = true;
	const char* tgt = (char*) target;
	unsigned tl = (unsigned) strlen(tgt);
	node->link = (char*) malloc(tl + 1);
	if (!node->link) { free(node); return E_NOSPC; }
	for (unsigned i = 0; i < tl; i++) node->link[i] = tgt[i];
	node->link[tl] = 0;
	node->mode = 0777;
	if (!addChild(parent, leaf, len, node)) { free(node->link); free(node); return E_NOSPC; }
	return 0;
}

int RamFs::truncate(String path, unsigned length) {
	RamNode* n = walkFollow((char*) path, 0);
	if (!n) return E_NOENT;
	if (n->isDir) return E_ISDIR;
	if (!ensureCap(n, length)) return E_NOSPC;
	if (length > n->size)
		memset(n->data + n->size, 0, length - n->size);
	n->size = length;
	return 0;
}

int RamFs::chmod(String path, unsigned mode) {
	RamNode* n = walkFollow((char*) path, 0);
	if (!n) return E_NOENT;
	n->mode = mode & 0777;
	return 0;
}

int RamFs::chown(String path, unsigned uid, unsigned gid) {
	RamNode* n = walkFollow((char*) path, 0);
	if (!n) return E_NOENT;
	if (uid != 0xFFFFFFFFu) n->uid = uid;
	if (gid != 0xFFFFFFFFu) n->gid = gid;
	return 0;
}

int RamFs::lchown(String path, unsigned uid, unsigned gid) {
	RamNode* n = walk((char*) path);            // no-follow: chown the link itself
	if (!n) return E_NOENT;
	if (uid != 0xFFFFFFFFu) n->uid = uid;
	if (gid != 0xFFFFFFFFu) n->gid = gid;
	return 0;
}

int RamFs::utimes(String path, unsigned atime, unsigned mtime) {
	RamNode* n = walkFollow((char*) path, 0);
	if (!n) return E_NOENT;
	n->atime = atime;
	n->mtime = mtime;
	return 0;
}

int RamFs::statfs(String path, StatFs& out) {
	(void) path;
	out.blockSize = 4096;
	out.totalBlocks = 0;
	out.freeBlocks = 0;
	out.totalInodes = 0;
	out.freeInodes = 0;
	out.nameMax = 63;
	return 0;
}

}
