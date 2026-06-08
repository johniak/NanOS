/*
 * NxeLoader.h
 *
 * Host-testable core of the .nx loader: applies base relocations (so a module can
 * load at any base), zeroes bss, binds the Import Address Table by resolving each
 * import name via a callback, and reports the module's exports. Pure memory
 * operations over the loaded image — no arch/VFS dependencies.
 */
#pragma once
#include "NxFormat.h"

namespace kernel {

// Resolve an import name to a function address (0 = not found).
typedef void* (*ExportResolver)(const char* name);

// What loadImage reports back: the (relocated) entry point plus the module's export
// table, which the dynamic linker uses to satisfy other modules' imports by name.
struct NxLoaded {
	unsigned entry;            // relocated entry point (.nxe)
	void* image;               // the loaded buffer (base of the file image)
	unsigned loadBase;         // header.loadBase (subtract to turn a vaddr into a buffer offset)
	unsigned loadDelta;        // applied delta (actualBase - loadBase)
	const NxExport* exports;   // export table inside `image`; addrs already relocated
	unsigned exportCount;
};

class NxeLoader {
public:
	// `image` points to the loaded .nx bytes; `len` is the CAPACITY of that buffer (not
	// the file length): bss is zeroed in place past the stored bytes, so the buffer must
	// be large enough to hold the image plus its bss, and every access is bounds-checked
	// against `len`. `loadDelta` is (actualBase - header.loadBase): 0 when loaded at the
	// preferred base. Applies relocations, binds imports via `resolve` (may be 0 when
	// importCount==0), zeroes bss, and (if non-null) fills `out`. Returns 0 and *entryOut
	// on success, <0 on error.
	static int loadImage(void* image, unsigned len, unsigned loadDelta,
			ExportResolver resolve, unsigned* entryOut, NxLoaded* out = 0);

	// Translate an export's name offset (a vaddr relative to loadBase) into a string
	// pointer inside the loaded buffer.
	static const char* exportName(const NxLoaded& m, const NxExport& e) {
		return (const char*) m.image + (e.nameOff - m.loadBase);
	}
};

}
