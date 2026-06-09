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

// Load one .ndl into `space` at `base`, relocating it and registering its exports in this
// library's own `table`. Returns 0 on success, <0 on error.
int loadLibrary(Vfs* vfs, const char* name, unsigned base, arch::AddressSpace* space,
		SymTable* table) {
	char path[160];
	libPath(path, name);
	String p((char*) path);
	FileStat st;
	if (vfs->stat(p, st) < 0)
		return -1;
	// Peek the header to size the staging buffer (it must hold the image + its bss).
	NxHeader hdr;
	if (vfs->read(p, sizeof hdr, 0, &hdr) < 0 || hdr.magic != NX_MAGIC)
		return -1;
	unsigned imageBytes = hdr.bssEnd - hdr.loadBase;        // mapped extent (image + bss)
	unsigned cap = st.size > imageBytes ? st.size : imageBytes;
	cap = (cap + 0xFFF) & ~0xFFFu;
	unsigned char* buf = new unsigned char[cap];
	for (unsigned i = 0; i < cap; i++)
		buf[i] = 0;
	if (vfs->read(p, st.size, 0, buf) < 0) {
		delete[] buf;
		return -1;
	}
	unsigned delta = base - hdr.loadBase;                   // relocate to the assigned base
	unsigned entry = 0;
	int rc = NxeLoader::loadImage(buf, cap, delta, resolveSym, &entry, onExport, table);
	if (rc < 0) {
		delete[] buf;
		return rc;
	}
	arch::archLoadModule(space, base, buf, imageBytes);     // map the relocated image
	delete[] buf;
	return 0;
}

}  // namespace

int dynLoadProgram(Vfs* vfs, void* exeImage, unsigned exeCap,
		arch::AddressSpace* space, unsigned* entryOut) {
	LibSet* libs = new LibSet();
	libs->init();
	g_libs = libs;

	// Discover the needed libraries (read-only; the image is bound below).
	NeedList needed;
	needed.n = 0;
	int rc = NxeLoader::forEachNeeded(exeImage, exeCap, onNeeded, &needed);

	// Load each needed .ndl into its own module window, each building its OWN export table
	// (keyed by soname) so imports resolve per-library.
	unsigned base = arch::mmuModuleBase();
	for (int i = 0; rc == 0 && i < needed.n; i++) {
		if (base >= arch::mmuModuleMax()) { rc = -1; break; }   // out of module windows
		SymTable* t = libs->create(needed.names[i]);
		if (!t) { rc = -1; break; }
		rc = loadLibrary(vfs, needed.names[i], base, space, t);
		base += arch::mmuModuleStride();
	}

	// Finally bind the executable's imports (each scoped to its declared library) — delta 0,
	// the executable keeps its preferred base. This also relocates + zeroes the EXE's bss.
	if (rc == 0)
		rc = NxeLoader::loadImage(exeImage, exeCap, 0, resolveSym, entryOut, 0, 0);

	g_libs = 0;
	for (int i = 0; i < libs->n; i++)
		delete libs->tables[i];
	delete libs;
	return rc;
}

}
