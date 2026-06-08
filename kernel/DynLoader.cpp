#include "DynLoader.h"
#include "NxeLoader.h"
#include "NxFormat.h"
#include "Vfs.h"
#include "String.h"
#include <arch/mmu.h>
#include <arch/usermode.h>

namespace kernel {

namespace {

// The flat symbol table active during a single dynLoadProgram. Loading is serial (exec
// runs to completion before another starts), so a file-static pointer is enough to give
// NxeLoader's C-style resolver/visitor callbacks access to it.
SymTable* g_table = 0;

void* resolveSym(const char* name) {
	return (void*) (g_table ? g_table->find(name) : 0);
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

// Load one .ndl into `space` at `base`, relocating it and registering its exports in
// `table`. Returns 0 on success, <0 on error.
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
	SymTable* table = new SymTable();
	g_table = table;

	// Discover the needed libraries (read-only; the image is bound below).
	NeedList needed;
	needed.n = 0;
	int rc = NxeLoader::forEachNeeded(exeImage, exeCap, onNeeded, &needed);

	// Load each needed .ndl into its own module window, building the symbol table.
	unsigned base = arch::mmuModuleBase();
	for (int i = 0; rc == 0 && i < needed.n; i++) {
		if (base >= arch::mmuModuleMax()) { rc = -1; break; }   // out of module windows
		rc = loadLibrary(vfs, needed.names[i], base, space, table);
		base += arch::mmuModuleStride();
	}

	// Finally bind the executable's imports against the flat symbol table (delta 0: the
	// executable keeps its preferred base). This also relocates + zeroes the EXE's bss.
	if (rc == 0)
		rc = NxeLoader::loadImage(exeImage, exeCap, 0, resolveSym, entryOut, 0, 0);

	g_table = 0;
	delete table;
	return rc;
}

}
