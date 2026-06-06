/*
 * NxeLoader.h
 *
 * Host-testable core of the .nx loader: validates the header, zeroes bss, and
 * binds the Import Address Table by resolving each import name via a callback.
 * Pure memory operations over the loaded image — no arch/VFS dependencies.
 */
#pragma once
#include "NxFormat.h"

namespace kernel {

// Resolve an import name to a function address (0 = not found).
typedef void* (*ExportResolver)(const char* name);

class NxeLoader {
public:
	// `image` points to the loaded .nx bytes (at header.loadBase in the kernel,
	// or anywhere on the host — absolute addresses are translated relative to the
	// buffer). Returns 0 and *entryOut on success, <0 on error.
	static int loadImage(void* image, unsigned len, ExportResolver resolve,
			unsigned* entryOut);
};

}
