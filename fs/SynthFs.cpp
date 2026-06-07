#include "SynthFs.h"
#include <string.h>

namespace kernel {

// Length-bounded name compare (avoids strncmp, absent from the freestanding libc):
// node name `a` (NUL-terminated) equals the `blen`-char component `b`.
static bool nameEq(const char* a, const char* b, int blen) {
	int i = 0;
	for (; i < blen; i++)
		if (a[i] == 0 || a[i] != b[i])
			return false;
	return a[i] == 0;
}

SynthNode* SynthFs::mk(SynthKind kind, const char* name, unsigned perms) {
	SynthNode* n = new SynthNode();
	int i = 0;
	for (; name[i] && i < 63; i++)
		n->name[i] = name[i];
	n->name[i] = 0;
	n->kind = kind;
	n->nchild = 0;
	n->data = 0;
	n->len = 0;
	n->gen = 0;
	n->perms = perms;
	return n;
}

SynthNode* SynthFs::addDir(SynthNode* parent, const char* name) {
	SynthNode* n = mk(SK_DIR, name, 0555);
	if (parent->nchild < 32)
		parent->child[parent->nchild++] = n;
	return n;
}

void SynthFs::addStatic(SynthNode* parent, const char* name, const char* data, unsigned len) {
	SynthNode* n = mk(SK_STATIC, name, 0444);
	n->data = data;
	n->len = len;
	if (parent->nchild < 32)
		parent->child[parent->nchild++] = n;
}

void SynthFs::addGen(SynthNode* parent, const char* name, SynthGen g, unsigned perms) {
	SynthNode* n = mk(SK_GEN, name, perms);
	n->gen = g;
	if (parent->nchild < 32)
		parent->child[parent->nchild++] = n;
}

void SynthFs::addVolume(const char* name) {
	addDir(m_disks, name);
}

// ---- generated nodes (devices + proc) -------------------------------------
// Stream devices ignore offset: null is always EOF; zero/random produce endlessly.
static int gen_null(unsigned, void*, unsigned) { return 0; }

static int gen_zero(unsigned, void* buf, unsigned n) {
	memset(buf, 0, n);
	return (int) n;
}

static int gen_random(unsigned, void* buf, unsigned n) {
	static unsigned state = 2463534242u;   // xorshift32, advances across reads
	unsigned char* p = (unsigned char*) buf;
	for (unsigned i = 0; i < n; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		p[i] = (unsigned char) state;
	}
	return (int) n;
}

// Snapshot file: serve a fixed string by offset so `cat` terminates (no RTC yet).
static int gen_uptime(unsigned off, void* buf, unsigned n) {
	static const char s[] = "uptime: 0 (no timer yet)\n";
	unsigned len = sizeof(s) - 1;
	if (off >= len)
		return 0;
	unsigned cnt = n < (len - off) ? n : (len - off);
	memcpy(buf, s + off, cnt);
	return (int) cnt;
}

SynthFs::SynthFs() {
	root = mk(SK_DIR, "/", 0555);
	m_disks = addDir(root, "disks");
	m_dev = addDir(root, "dev");
	m_proc = addDir(root, "proc");

	addGen(m_dev, "null", gen_null, 0666);
	addGen(m_dev, "zero", gen_zero, 0666);
	addGen(m_dev, "random", gen_random, 0444);
	addGen(m_proc, "uptime", gen_uptime, 0444);
}

int SynthFs::mount() { return 0; }

SynthNode* SynthFs::dirChild(SynthNode* d, const char* name, int len) {
	for (int i = 0; i < d->nchild; i++)
		if (nameEq(d->child[i]->name, name, len))
			return d->child[i];
	return 0;
}

// Walk an absolute path to its node, or 0 if missing. Empty components (leading,
// trailing, or doubled '/') are skipped, so "/", "/dev", "/dev/" all resolve.
SynthNode* SynthFs::walk(const char* path) {
	if (path[0] != '/')
		return 0;
	SynthNode* cur = root;
	int i = 1;
	while (path[i]) {
		int j = i;
		while (path[j] && path[j] != '/')
			j++;
		int len = j - i;
		if (len > 0) {
			if (cur->kind != SK_DIR)
				return 0;
			cur = dirChild(cur, path + i, len);
			if (!cur)
				return 0;
		}
		i = (path[j] == '/') ? j + 1 : j;
	}
	return cur;
}

int SynthFs::read(String path, unsigned size, unsigned off, void* buf) {
	SynthNode* n = walk((char*) path);
	if (!n)
		return -1;
	if (n->kind == SK_STATIC) {
		if (off >= n->len)
			return 0;
		unsigned avail = n->len - off;
		unsigned cnt = size < avail ? size : avail;
		memcpy(buf, n->data + off, cnt);
		return (int) cnt;
	}
	if (n->kind == SK_GEN)
		return n->gen(off, buf, size);
	return -1;   // directory
}

int SynthFs::stat(String path, FileStat& out) {
	SynthNode* n = walk((char*) path);
	if (!n)
		return -1;
	if (n->kind == SK_DIR) {
		out.type = NODE_DIR;
		out.size = 0;
		out.mode = 0x4000 | (n->perms & 0777);
	} else {
		out.type = NODE_FILE;
		out.size = (n->kind == SK_STATIC) ? n->len : 0;
		out.mode = 0x8000 | (n->perms & 0777);
	}
	out.nlink = 1;
	out.uid = 0;
	out.gid = 0;
	out.mtime = 0;
	return 0;
}

int SynthFs::readdir(String path, List<DirEntry>& out) {
	SynthNode* n = walk((char*) path);
	if (!n || n->kind != SK_DIR)
		return -1;
	for (int i = 0; i < n->nchild; i++) {
		DirEntry de;
		int k = 0;
		for (; n->child[i]->name[k] && k < 255; k++)
			de.name[k] = n->child[i]->name[k];
		de.name[k] = 0;
		de.type = (n->child[i]->kind == SK_DIR) ? NODE_DIR : NODE_FILE;
		out.add(de);
	}
	return 0;
}

}
