#include "NxeLoader.h"
#include <string.h>

namespace kernel {

// True if [abs, abs+sz) lies within the loaded image [base, base+len). Addresses are
// nxaddr_t (64-bit on x86_64), so the arithmetic never truncates a 64-bit fixup site.
static bool inImage(nxaddr_t abs, nxaddr_t sz, nxaddr_t base, unsigned len) {
	if (abs < base)
		return false;
	nxaddr_t off = abs - base;
	return off + sz <= len && off + sz >= off;
}

int NxeLoader::forEachNeeded(const void* image, unsigned len, NeededFn fn, void* ctx) {
	if (len < sizeof(NxHeader))
		return -1;
	const NxHeader* h = (const NxHeader*) image;
	if (h->magic != NX_MAGIC || h->version != NX_VERSION)
		return -1;
	const char* img = (const char*) image;
	nxaddr_t base = h->loadBase;
	if (!h->neededCount)
		return 0;
	if (!inImage(h->neededTable, (nxaddr_t) h->neededCount * sizeof(NxNeeded), base, len))
		return -3;
	const NxNeeded* nd = (const NxNeeded*) (img + (h->neededTable - base));
	for (unsigned i = 0; i < h->neededCount; i++) {
		if (!inImage(nd[i].nameOff, 1, base, len))
			return -3;
		if (fn)
			fn(ctx, img + (nd[i].nameOff - base));
	}
	return 0;
}

int NxeLoader::loadImage(void* image, unsigned len, nxaddr_t loadDelta,
		ExportResolver resolve, nxaddr_t* entryOut, ExportFn onExport, void* ctx) {
	if (len < sizeof(NxHeader))
		return -1;
	NxHeader* h = (NxHeader*) image;
	// Reject anything that is not our exact format/version (clean cut: the x86_64 loader
	// never reads a v3 i386 image, and vice versa — addresses would be the wrong width).
	if (h->magic != NX_MAGIC || h->version != NX_VERSION)
		return -1;
	char* img = (char*) image;
	nxaddr_t base = h->loadBase;

	// 1) Base relocations: each listed absolute is the natural word (R_X86_64_64 / 8 bytes on
	//    x86_64, R_386_32 / 4 bytes on i386), UNLESS the offset carries NX_RELOC_W32 — then it
	//    is a 4-byte (R_X86_64_32S) site that small-model non-PIC library code uses to address
	//    a symbol. Either way the loader adds the load delta. A delta of 0 (loaded at the
	//    preferred base, e.g. the executable) makes this a no-op.
	if (h->relocCount) {
		if (!inImage(h->relocTable, (nxaddr_t) h->relocCount * sizeof(NxReloc), base, len))
			return -3;
		NxReloc* rel = (NxReloc*) (img + (h->relocTable - base));
		for (unsigned i = 0; i < h->relocCount; i++) {
			nxaddr_t off = rel[i].off & ~NX_RELOC_W32;
			int w32 = (rel[i].off & NX_RELOC_W32) != 0;
			nxaddr_t sz = w32 ? 4 : sizeof(nxaddr_t);
			if (!inImage(off, sz, base, len))
				return -3;
			if (w32)
				*(uint32_t*) (img + (off - base)) += (uint32_t) loadDelta;
			else
				*(nxaddr_t*) (img + (off - base)) += loadDelta;
		}
	}

	// 2) Bind imports: resolve each name and patch its IAT slot (a function pointer — 8
	//    bytes on x86_64, 4 on i386).
	if (h->importCount) {
		if (!inImage(h->importTable, (nxaddr_t) h->importCount * sizeof(NxImport), base, len))
			return -3;
		NxImport* imp = (NxImport*) (img + (h->importTable - base));
		for (unsigned i = 0; i < h->importCount; i++) {
			if (!inImage(imp[i].nameOff, 1, base, len)
					|| !inImage(imp[i].slotAddr, sizeof(void*), base, len))
				return -3;
			if (imp[i].libOff && !inImage(imp[i].libOff, 1, base, len))
				return -3;
			const char* name = (const char*) (img + (imp[i].nameOff - base));
			const char* lib = imp[i].libOff ? (const char*) (img + (imp[i].libOff - base)) : "";
			void* addr = resolve ? resolve(name, lib) : 0;
			if (addr == 0)
				return -2;
			*(void**) (img + (imp[i].slotAddr - base)) = addr;
		}
	}

	// 3) Exports: relocate each exported address and report it. Done before bss-zeroing,
	//    since the export table/strings can share the bss vaddr range.
	if (h->exportCount) {
		if (!inImage(h->exportTable, (nxaddr_t) h->exportCount * sizeof(NxExport), base, len))
			return -3;
		NxExport* ex = (NxExport*) (img + (h->exportTable - base));
		for (unsigned i = 0; i < h->exportCount; i++) {
			if (!inImage(ex[i].nameOff, 1, base, len))
				return -3;
			ex[i].addr += loadDelta;
			if (onExport)
				onExport(ctx, img + (ex[i].nameOff - base), ex[i].addr);
		}
	}

	// 4) Zero bss LAST: the tables above can live in the bss vaddr range (they are stored
	//    after the load image and consumed at load time, never mapped at runtime).
	if (h->bssEnd > h->bssStart) {
		if (!inImage(h->bssStart, h->bssEnd - h->bssStart, base, len))
			return -3;
		memset(img + (h->bssStart - base), 0, (size_t) (h->bssEnd - h->bssStart));
	}

	if (entryOut)
		*entryOut = h->entry + loadDelta;
	return 0;
}

}
