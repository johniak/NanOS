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

int NxeLoader::loadImage(void* image, unsigned len, ExportResolver resolve,
		unsigned* entryOut) {
	if (len < sizeof(NxHeader))
		return -1;
	NxHeader* h = (NxHeader*) image;
	if (h->magic != NX_MAGIC)
		return -1;
	char* img = (char*) image;
	unsigned base = h->loadBase;

	// Zero bss.
	if (h->bssEnd > h->bssStart) {
		if (!inImage(h->bssStart, h->bssEnd - h->bssStart, base, len))
			return -3;
		memset(img + (h->bssStart - base), 0, h->bssEnd - h->bssStart);
	}

	// Bind imports: resolve each name and patch its IAT slot.
	if (!inImage(h->importTable, h->importCount * sizeof(NxImport), base, len))
		return -3;
	NxImport* imp = (NxImport*) (img + (h->importTable - base));
	for (unsigned i = 0; i < h->importCount; i++) {
		if (!inImage(imp[i].nameOff, 1, base, len)
				|| !inImage(imp[i].slotAddr, sizeof(void*), base, len))
			return -3;
		const char* name = (const char*) (img + (imp[i].nameOff - base));
		void* addr = resolve(name);
		if (addr == 0)
			return -2;
		*(void**) (img + (imp[i].slotAddr - base)) = addr;
	}

	if (entryOut)
		*entryOut = h->entry;
	return 0;
}

}
