/*
 * KextLoader.h — loader for NanOS kernel modules (.nkext): pulls a module image from the VFS
 * into page-aligned kernel memory, relocates it, binds its imports against the kernel export
 * table (KernelExports), and calls its nkext_init() entry. Modules run in ring 0 in the
 * kernel address space (identity-mapped, executable). Reuses NxeLoader (the same .nxe/.ndl
 * relocate+bind engine) — the kext is just an NxFormat module whose imports resolve to the
 * kernel instead of to libc.ndl.
 */
#pragma once
#include "NxeLoader.h"   // ExportResolver

namespace kernel {

class Vfs;

// Load every "*.nkext" under `dir` (e.g. /disks/main/nanos/kext), calling each module's
// nkext_init(). Returns the number successfully loaded. Logs each load/failure.
int loadAllKexts(Vfs* vfs, const char* dir);

// Load one module file. Returns 0 on success (its nkext_init returned 0), <0 on error.
int loadKext(Vfs* vfs, const char* path);

// Host-testable core: relocate + bind an in-memory module image (delta derived from the
// buffer's address vs the header's preferred base) and return its entry. No VFS, no alloc.
// Returns 0 and *entryOut on success, <0 on error (same codes as NxeLoader::loadImage).
int loadKextImage(void* buf, unsigned cap, ExportResolver resolve, unsigned* entryOut);

}  // namespace kernel
