#include "KextLoader.h"
#include "KernelExports.h"
#include "NxFormat.h"
#include "Vfs.h"
#include "Console.h"
#include "List.h"
#include <string.h>

namespace kernel {

// Resident-module registry: a loaded module's image buffer is KEPT (not freed) so the module
// stays mapped, and recorded here — the foundation for a future rmmod/unload. We never leak
// silently: every loaded module has an entry.
struct KextModule {
	char name[64];
	void* image;      // page-aligned module base (the relocated NxFormat image)
	void* alloc;      // the raw allocation to free on unload
	unsigned size;
};
static const int MAXKEXT = 16;
static KextModule g_mods[MAXKEXT];
static int g_modCount = 0;

int loadKextImage(void* buf, unsigned cap, ExportResolver resolve, nxaddr_t* entryOut) {
	if (cap < sizeof(NxHeader))
		return -1;
	NxHeader* h = (NxHeader*) buf;
	if (h->magic != NX_MAGIC)
		return -1;
	unsigned delta = (unsigned) (unsigned long) buf - h->loadBase;
	return NxeLoader::loadImage(buf, cap, delta, resolve, entryOut, 0, 0);
}

int loadKext(Vfs* vfs, const char* path) {
	String p((char*) path);
	FileStat st;
	if (vfs->stat(p, st) < 0)
		return -1;
	// Peek the header to size the buffer (must hold the image + its bss).
	NxHeader hdr;
	if (vfs->read(p, sizeof hdr, 0, &hdr) < 0 || hdr.magic != NX_MAGIC)
		return -1;
	unsigned imageBytes = hdr.bssEnd - hdr.loadBase;
	unsigned cap = st.size > imageBytes ? st.size : imageBytes;
	cap = (cap + 0xFFFu) & ~0xFFFu;
	// Page-aligned module memory (over-allocate + align up). Identity-mapped + executable
	// (i686, no NX), so the relocated code runs in place at ring 0.
	unsigned char* raw = new unsigned char[cap + 0x1000];
	if (!raw)
		return -1;
	unsigned char* buf = (unsigned char*) (((unsigned long) raw + 0xFFFu) & ~0xFFFul);
	for (unsigned i = 0; i < cap; i++)
		buf[i] = 0;
	if (vfs->read(p, st.size, 0, buf) < 0) {
		delete[] raw;
		return -1;
	}
	nxaddr_t entry = 0;
	int rc = loadKextImage(buf, cap, kernelResolveSym, &entry);
	if (rc < 0) {
		delete[] raw;
		return rc;   // -2 = an import the kernel does not export (caught loudly, never silent)
	}
	// Record the resident module (basename of path).
	if (g_modCount < MAXKEXT) {
		KextModule& m = g_mods[g_modCount++];
		const char* b = path;
		for (int k = 0; path[k]; k++)
			if (path[k] == '/')
				b = path + k + 1;
		int i = 0;
		while (b[i] && i < 63) { m.name[i] = b[i]; i++; }
		m.name[i] = 0;
		m.image = buf;
		m.alloc = raw;
		m.size = cap;
	}
	// Enter the module: nkext_init() runs in ring 0 and registers IRQs / devices.
	int (*init)() = (int (*)()) entry;
	return init();
}

int loadAllKexts(Vfs* vfs, const char* dir) {
	String d((char*) dir);
	List<DirEntry> entries;
	if (vfs->readdir(d, entries) < 0)
		return 0;
	int loaded = 0;
	for (int i = 0; i < entries.getCount(); i++) {
		const char* nm = entries[i].name;
		int len = (int) strlen(nm);
		if (len < 6 || strcmp(nm + len - 6, ".nkext") != 0)
			continue;
		char path[256];
		int o = 0;
		for (int k = 0; dir[k] && o < 250; k++) path[o++] = dir[k];
		if (o == 0 || path[o - 1] != '/') path[o++] = '/';
		for (int k = 0; nm[k] && o < 255; k++) path[o++] = nm[k];
		path[o] = 0;
		Console::write("  kext: ");
		Console::write(nm);
		if (loadKext(vfs, path) == 0) {
			Console::writeLine(" [loaded]");
			loaded++;
		} else {
			Console::writeLine(" [FAILED]");
		}
	}
	return loaded;
}

}  // namespace kernel
