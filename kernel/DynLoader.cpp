#include "DynLoader.h"
#include "NxeLoader.h"
#include "NxFormat.h"
#include "Vfs.h"
#include "String.h"
#include <arch/mmu.h>
#include <arch/usermode.h>

namespace kernel {

namespace {

// Per-DLL namespace: one export SymTable per loaded library, keyed by soname. An import
// names its source library (NxImport.libOff) and is resolved against THAT library's table
// (the Windows-PE model); a nameless import (libOff==0 -> lib=="") falls back to a flat
// search across all loaded libraries.
struct LibSet {
	static const int MAX = 8;
	char names[MAX][32];
	SymTable* tables[MAX];
	int n;

	void init() { n = 0; }
	bool streq(const char* a, const char* b) {
		while (*a && *a == *b) { a++; b++; }
		return *a == *b;
	}
	bool has(const char* name) {                    // is this library already loaded?
		for (int i = 0; i < n; i++)
			if (streq(names[i], name))
				return true;
		return false;
	}
	SymTable* create(const char* name) {           // table for a newly loaded library
		if (n >= MAX)
			return 0;
		int i = 0;
		while (name[i] && i < 31) { names[n][i] = name[i]; i++; }
		names[n][i] = 0;
		tables[n] = new SymTable();
		return tables[n++];
	}
	unsigned find(const char* name, const char* lib) const {
		if (lib && lib[0]) {                        // scoped to the named library
			for (int i = 0; i < n; i++)
				if (((LibSet*) this)->streq(names[i], lib))
					return tables[i]->find(name);
			return 0;
		}
		for (int i = 0; i < n; i++) {               // flat fallback
			unsigned a = tables[i]->find(name);
			if (a)
				return a;
		}
		return 0;
	}
};

// Active during a single dynLoadProgram (exec is serial), so a file-static pointer
// suffices for NxeLoader's C-style resolver callback.
LibSet* g_libs = 0;

void* resolveSym(const char* name, const char* lib) {
	return (void*) (g_libs ? g_libs->find(name, lib) : 0);
}
void onExport(void* ctx, const char* name, unsigned addr) {
	((SymTable*) ctx)->add(name, addr);
}

// Needed-library names copied out of the executable image (it gets reused as we load).
struct NeedList { char names[8][64]; int n; };
void onNeeded(void* ctx, const char* name) {
	NeedList* nl = (NeedList*) ctx;
	if (nl->n >= 8)
		return;
	int i = 0;
	while (name[i] && i < 63) { nl->names[nl->n][i] = name[i]; i++; }
	nl->names[nl->n][i] = 0;
	nl->n++;
}

// "/disks/main/nanos/lib/<name>" into `out` (>= 160 bytes).
void libPath(char* out, const char* name) {
	const char* pre = "/disks/main/nanos/lib/";
	int o = 0;
	for (int i = 0; pre[i]; i++) out[o++] = pre[i];
	for (int i = 0; name[i]; i++) out[o++] = name[i];
	out[o] = 0;
}

// Per-exec loader state (exec is serial, so file-static is fine). g_nextBase hands out the
// next module window; loaded libraries are deduped via g_libs (a name already in the LibSet
// is already mapped + bound).
Vfs* g_vfs = 0;
arch::AddressSpace* g_space = 0;
unsigned g_nextBase = 0;

// Recursively ensure library `name` and its WHOLE dependency graph are loaded, dependencies
// first (post-order) — the Windows/dyld model: a .ndl declares the libraries IT needs, and
// the loader walks the graph, so a client that links libnw.ndl gets libc.ndl automatically.
// Each library binds its imports against everything loaded so far and registers its exports
// in its own soname-keyed table. Diamonds load once (deduped by g_libs).
int ensureLib(const char* name) {
	if (g_libs->has(name))
		return 0;                                           // already loaded
	char path[160];
	libPath(path, name);
	String p((char*) path);
	FileStat st;
	if (g_vfs->stat(p, st) < 0)
		return -1;
	NxHeader hdr;
	if (g_vfs->read(p, sizeof hdr, 0, &hdr) < 0 || hdr.magic != NX_MAGIC)
		return -1;
	unsigned imageBytes = hdr.bssEnd - hdr.loadBase;        // mapped extent (image + bss)
	unsigned cap = st.size > imageBytes ? st.size : imageBytes;
	cap = (cap + 0xFFF) & ~0xFFFu;
	unsigned char* buf = new unsigned char[cap];
	for (unsigned i = 0; i < cap; i++)
		buf[i] = 0;
	if (g_vfs->read(p, st.size, 0, buf) < 0) {
		delete[] buf;
		return -1;
	}
	// Load THIS library's own needed libraries first (transitive resolution).
	NeedList sub;
	sub.n = 0;
	int rc = NxeLoader::forEachNeeded(buf, cap, onNeeded, &sub);
	for (int i = 0; rc == 0 && i < sub.n; i++)
		rc = ensureLib(sub.names[i]);
	if (rc < 0) {
		delete[] buf;
		return rc;
	}
	if (g_nextBase >= arch::mmuModuleMax()) {               // out of module windows
		delete[] buf;
		return -1;
	}
	unsigned base = g_nextBase;
	g_nextBase += arch::mmuModuleStride();
	SymTable* table = g_libs->create(name);
	if (!table) {
		delete[] buf;
		return -1;
	}
	unsigned entry = 0;
	rc = NxeLoader::loadImage(buf, cap, base - hdr.loadBase, resolveSym, &entry, onExport, table);
	if (rc == 0)
		arch::archLoadModule(g_space, base, buf, imageBytes);
	delete[] buf;
	return rc;
}

}  // namespace

int dynLoadProgram(Vfs* vfs, void* exeImage, unsigned exeCap,
		arch::AddressSpace* space, unsigned* entryOut) {
	LibSet* libs = new LibSet();
	libs->init();
	g_libs = libs;
	g_vfs = vfs;
	g_space = space;
	g_nextBase = arch::mmuModuleBase();

	// Discover + recursively load the executable's needed libraries (each + its deps).
	NeedList needed;
	needed.n = 0;
	int rc = NxeLoader::forEachNeeded(exeImage, exeCap, onNeeded, &needed);
	for (int i = 0; rc == 0 && i < needed.n; i++)
		rc = ensureLib(needed.names[i]);

	// Finally bind the executable's imports (each scoped to its declared library) — delta 0,
	// the executable keeps its preferred base. This also relocates + zeroes the EXE's bss.
	if (rc == 0)
		rc = NxeLoader::loadImage(exeImage, exeCap, 0, resolveSym, entryOut, 0, 0);

	g_libs = 0;
	g_vfs = 0;
	g_space = 0;
	for (int i = 0; i < libs->n; i++)
		delete libs->tables[i];
	delete libs;
	return rc;
}

}
