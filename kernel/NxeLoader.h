/*
 * NxeLoader.h
 *
 * Host-testable core of the .nx loader: applies base relocations (so a module can
 * load at any base), binds the Import Address Table by resolving each import name via a
 * callback, reports the module's exports + needed libraries, and zeroes bss. Pure memory
 * operations over the loaded image — no arch/VFS dependencies.
 *
 * The reloc/import/export/needed tables are stored after the load image, sharing the bss
 * vaddr range; the loader consumes them BEFORE zeroing bss. Exports and needed names are
 * therefore delivered via callbacks (their backing bytes are clobbered by the bss-zero,
 * so a caller that wants to keep a name must copy it during the callback).
 */
#pragma once
#include "NxFormat.h"

namespace kernel {

// Resolve an import to an address (0 = not found). `lib` names the source library the
// import declared (per-DLL namespace); "" means resolve flat across all modules.
typedef void* (*ExportResolver)(const char* name, const char* lib);
// Visit one exported symbol (name valid only for the duration of the call).
typedef void (*ExportFn)(void* ctx, const char* name, unsigned addr);
// Visit one needed-library name (valid only for the duration of the call).
typedef void (*NeededFn)(void* ctx, const char* name);

class NxeLoader {
public:
	// Read-only: validate the header and visit each needed-library name. Does not modify
	// the buffer (safe to call before loadImage to discover which .ndl files to load).
	// Returns 0 on success, <0 on a malformed header/table.
	static int forEachNeeded(const void* image, unsigned len, NeededFn fn, void* ctx);

	// `image` points to the loaded .nx bytes; `len` is the CAPACITY of that buffer (not
	// the file length): bss is zeroed in place past the stored bytes, so the buffer must
	// be large enough to hold the image plus its bss, and every access is bounds-checked
	// against `len`. `loadDelta` is (actualBase - header.loadBase): 0 when loaded at the
	// preferred base. Applies relocations, binds imports via `resolve` (may be 0 when
	// importCount==0), visits each export (relocated address) via `onExport` (may be 0),
	// then zeroes bss. Returns 0 and *entryOut on success, <0 on error.
	static int loadImage(void* image, unsigned len, unsigned loadDelta,
			ExportResolver resolve, unsigned* entryOut,
			ExportFn onExport = 0, void* ctx = 0);
};

}
