#include "NxeLoader.h"
#include <string.h>

namespace kernel {

// True if [abs, abs+sz) lies within the loaded image [base, base+len).
static bool inImage(unsigned abs, unsigned sz, unsigned base, unsigned len) {
	if (abs < base)
		return false;
	unsigned off = abs - base;
	return off + sz <= len && off + sz >= off;
}

int NxeLoader::loadImage(void* image, unsigned len, unsigned loadDelta,
		ExportResolver resolve, unsigned* entryOut, NxLoaded* out) {
	if (len < sizeof(NxHeader))
		return -1;
	NxHeader* h = (NxHeader*) image;
	if (h->magic != NX_MAGIC)
		return -1;
	char* img = (char*) image;
	unsigned base = h->loadBase;

	// 1) Base relocations: add the load delta to each listed absolute (R_386_32) word.
	//    A delta of 0 (loaded at the preferred base) makes this a no-op.
	if (h->relocCount) {
		if (!inImage(h->relocTable, h->relocCount * sizeof(NxReloc), base, len))
			return -3;
		NxReloc* rel = (NxReloc*) (img + (h->relocTable - base));
		for (unsigned i = 0; i < h->relocCount; i++) {
			if (!inImage(rel[i].off, 4, base, len))
				return -3;
			*(unsigned*) (img + (rel[i].off - base)) += loadDelta;
		}
	}

	// 2) Bind imports: resolve each name and patch its IAT slot. (Before bss-zeroing in
	//    case a table or slot shares the post-image vaddr range.)
	if (h->importCount) {
		if (!inImage(h->importTable, h->importCount * sizeof(NxImport), base, len))
			return -3;
		NxImport* imp = (NxImport*) (img + (h->importTable - base));
		for (unsigned i = 0; i < h->importCount; i++) {
			if (!inImage(imp[i].nameOff, 1, base, len)
					|| !inImage(imp[i].slotAddr, sizeof(void*), base, len))
				return -3;
			const char* name = (const char*) (img + (imp[i].nameOff - base));
			void* addr = resolve ? resolve(name) : 0;
			if (addr == 0)
				return -2;
			*(void**) (img + (imp[i].slotAddr - base)) = addr;
		}
	}

	// 3) Exports: relocate each exported address so callers see final addresses.
	const NxExport* exports = 0;
	if (h->exportCount) {
		if (!inImage(h->exportTable, h->exportCount * sizeof(NxExport), base, len))
			return -3;
		NxExport* ex = (NxExport*) (img + (h->exportTable - base));
		for (unsigned i = 0; i < h->exportCount; i++) {
			if (!inImage(ex[i].nameOff, 1, base, len))
				return -3;
			ex[i].addr += loadDelta;
		}
		exports = ex;
	}

	// 4) Zero bss LAST: the tables above can live in the bss vaddr range (they are stored
	//    after the load image and consumed at load time, never mapped at runtime).
	if (h->bssEnd > h->bssStart) {
		if (!inImage(h->bssStart, h->bssEnd - h->bssStart, base, len))
			return -3;
		memset(img + (h->bssStart - base), 0, h->bssEnd - h->bssStart);
	}

	unsigned entry = h->entry + loadDelta;
	if (entryOut)
		*entryOut = entry;
	if (out) {
		out->entry = entry;
		out->image = image;
		out->loadBase = base;
		out->loadDelta = loadDelta;
		out->exports = exports;
		out->exportCount = h->exportCount;
	}
	return 0;
}

}
