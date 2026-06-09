/*
 * RamFs.cpp — the in-memory writable filesystem (tmpfs). See RamFs.h.
 *
 * A small tree of RamNodes walked by path. Directories hold a fixed child array; files
 * hold a malloc'd, geometrically-grown data buffer. Errors use the kernel's negative
 * errno convention (-ENOENT etc.), matching the rest of the VFS.
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

bool nameEq(const char* a, const char* b, int len) {
	for (int i = 0; i < len; i++)
		if (a[i] != b[i] || a[i] == 0)
			return false;
	return a[len] == 0;
}
}

RamFs::RamFs() {
	root = mk("", 0, true);
}

int RamFs::mount() { return 0; }

RamNode* RamFs::mk(const char* name, int len, bool isDir) {
	RamNode* n = (RamNode*) malloc(sizeof(RamNode));
	memset(n, 0, sizeof(RamNode));
	if (len > 63)
		len = 63;
	for (int i = 0; i < len; i++)
		n->name[i] = name[i];
	n->name[len] = 0;
	n->isDir = isDir;
	return n;
}

// Append a child to a directory, geometrically growing the child array (tmpfs has no
// per-directory entry limit, so neither do we — no silent -ENOSPC at a magic count).
bool RamFs::addChild(RamNode* d, RamNode* c) {
	if (d->nchild >= d->childCap) {
		int cap = d->childCap ? d->childCap * 2 : 8;
		RamNode** p = (RamNode**) realloc(d->child, (unsigned) cap * sizeof(RamNode*));
		if (!p)
			return false;
		d->child = p;
		d->childCap = cap;
	}
	d->child[d->nchild++] = c;
	return true;
}

RamNode* RamFs::dirChild(RamNode* d, const char* name, int len) {
	if (!d->isDir)
		return 0;
	for (int i = 0; i < d->nchild; i++)
		if (nameEq(d->child[i]->name, name, len))
			return d->child[i];
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

// Resolve the parent directory of the last component. Sets leaf/leafLen to the final
// name. Returns 0 if the path is malformed, the parent is missing, or it is not a dir.
RamNode* RamFs::walkParent(const char* path, const char*& leaf, int& leafLen) {
	if (!path || path[0] != '/')
		return 0;
	// Find the last non-empty component.
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
	// Walk the directory prefix [0, start).
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
	RamNode* n = walk((char*) path);
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
	RamNode* n = walk((char*) path);
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

int RamFs::create(String path, unsigned /*mode*/) {
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
	RamNode* node = mk(leaf, len, false);
	if (!node || !addChild(parent, node)) {
		if (node) free(node);
		return E_NOSPC;
	}
	return 0;
}

int RamFs::mkdir(String path, unsigned /*mode*/) {
	const char* leaf;
	int len;
	RamNode* parent = walkParent((char*) path, leaf, len);
	if (!parent)
		return E_NOENT;
	if (dirChild(parent, leaf, len))
		return E_EXIST;
	RamNode* node = mk(leaf, len, true);
	if (!node || !addChild(parent, node)) {
		if (node) free(node);
		return E_NOSPC;
	}
	return 0;
}

int RamFs::unlink(String path) {
	const char* leaf;
	int len;
	RamNode* parent = walkParent((char*) path, leaf, len);
	if (!parent)
		return E_NOENT;
	for (int i = 0; i < parent->nchild; i++) {
		RamNode* c = parent->child[i];
		if (nameEq(c->name, leaf, len)) {
			if (c->isDir)
				return E_ISDIR;              // unlink targets files; rmdir is separate
			if (c->data)
				free(c->data);
			free(c);
			for (int j = i; j < parent->nchild - 1; j++)   // compact the child array
				parent->child[j] = parent->child[j + 1];
			parent->nchild--;
			return 0;
		}
	}
	return E_NOENT;
}

int RamFs::stat(String path, FileStat& out) {
	RamNode* n = walk((char*) path);
	if (!n)
		return E_NOENT;
	out.type = n->isDir ? NODE_DIR : NODE_FILE;
	out.size = n->isDir ? 0 : n->size;
	out.mode = n->isDir ? 0x41EDu : 0x81B6u;   // 0755 dir / 0666 file
	out.nlink = 1;
	out.uid = 0;
	out.gid = 0;
	out.mtime = n->mtime;
	return 0;
}

int RamFs::readdir(String path, List<DirEntry>& out) {
	RamNode* n = walk((char*) path);
	if (!n)
		return E_NOENT;
	if (!n->isDir)
		return E_NOTDIR;
	// "." and ".." first (so ls shows them, matching SynthFs and a real Unix readdir).
	DirEntry dot;
	dot.type = NODE_DIR;
	dot.name[0] = '.'; dot.name[1] = 0;
	out.add(dot);
	dot.name[1] = '.'; dot.name[2] = 0;
	out.add(dot);
	for (int i = 0; i < n->nchild; i++) {
		DirEntry e;
		int k = 0;
		for (; n->child[i]->name[k] && k < 255; k++)
			e.name[k] = n->child[i]->name[k];
		e.name[k] = 0;
		e.type = n->child[i]->isDir ? NODE_DIR : NODE_FILE;
		out.add(e);
	}
	return 0;
}

}
