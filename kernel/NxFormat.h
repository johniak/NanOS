/*
 * NxFormat.h
 *
 * The NanOS binary format family (magic "NXE"). One header + import-by-name +
 * export-by-name + base-relocation mechanism, shared by:
 *   .nxe   Nano Executable        (programs)
 *   .ndl   Nano Dynamic Library   (shared libraries)
 *   .nkext Nano Kernel Extension  (loadable kernel modules — planned)
 *
 * This is the Windows-PE-style dynamic-link model: a module is linked at a preferred
 * base but can load anywhere — the loader adds (actualBase - preferredBase) to every
 * address listed in the relocation table (R_386_32 absolute fixups). Executables import
 * functions by name from named DLLs (the loader patches each import's IAT slot to the
 * resolved address); DLLs export functions by name. So a program binary depends on a
 * stable named API, not a baked-in library — no "binary per distro" problem.
 *
 * Shared by the kernel loader and the build tool (tools/mknx). Plain C (no namespace).
 *
 * Layout on disk (and in memory when loaded at header.loadBase):
 *   [ NxHeader | code | rodata | data | NxImport[] | NxExport[] | NxReloc[] |
 *     NxNeeded[] | strings | (bss, not stored) ]
 * All table addresses in the header are ABSOLUTE (relative to loadBase).
 */
#ifndef NXFORMAT_H
#define NXFORMAT_H
#include <stdint.h>

#define NX_MAGIC   0x0045584E   /* 'N','X','E',0 little-endian */
#define NX_FLAG_DLL 1u          /* header.flags bit 0: module is a shared library */

/* Address width is machine-dependent. On x86_64 (and under -DNX_FORCE64 for the host-
 * compiled mknx64 / unit tests) the module's absolute addresses are 64-bit and the format
 * is version 4; on i386 they stay 32-bit and the format is version 3. This is a clean cut
 * (the x86_64 loader never reads a v3 image at runtime), and i686 keeps building/working
 * unchanged through the whole transition. Counts are not addresses, so they are a fixed
 * uint32_t on both sides. */
#if defined(__x86_64__) || defined(NX_FORCE64)
#define NX_VERSION 4
typedef uint64_t nxaddr_t;      /* absolute module addresses are 64-bit on x86_64 */
#else
#define NX_VERSION 3
typedef uint32_t nxaddr_t;      /* i386: 32-bit, identical layout to the historical format */
#endif

/* An imported symbol: resolve `nameOff` against the exports of the module named `libOff`
 * (per-DLL namespace, the Windows-PE model), then store the address into the IAT slot at
 * `slotAddr`. `libOff` == 0 means "no named library" — resolve flat across all modules. */
typedef struct {
	nxaddr_t nameOff;    /* abs address of the import's NUL-terminated name */
	nxaddr_t slotAddr;   /* abs address of the IAT slot (a function pointer) to patch */
	nxaddr_t libOff;     /* abs address of the source library's name, or 0 = flat */
} NxImport;

/* An exported symbol: `nameOff` is callable at address `addr` once the module is loaded
 * (and relocated). */
typedef struct {
	nxaddr_t nameOff;    /* abs address of the export's NUL-terminated name */
	nxaddr_t addr;       /* abs address of the exported symbol */
} NxExport;

/* A base relocation: the word at `off` holds an absolute address (R_386_32 on i386,
 * R_X86_64_64 on x86_64); the loader adds the load delta to it when the module loads at a
 * non-preferred base.
 *
 * On x86_64 the top bit of `off` (NX_RELOC_W32) marks a 4-byte (R_X86_64_32S) site instead
 * of the natural 8-byte word: small-model non-PIC code addresses symbols with 32-bit
 * absolutes, and those still shift with the load base when a LIBRARY (.ndl) is relocated to
 * a non-preferred base. The executable loads at delta 0 so its 32-bit sites are no-ops, but a
 * library must fix them too. i686 never sets the bit (all its relocs are 4-byte R_386_32 ==
 * the natural word, and modules load well below the 2 GiB mark where the bit would live). */
typedef struct {
	nxaddr_t off;        /* abs address of the word to fix up (top bit = NX_RELOC_W32 tag) */
} NxReloc;
#define NX_RELOC_W32  (((nxaddr_t) 1) << (8 * sizeof(nxaddr_t) - 1))

/* A needed shared library: load the .ndl named `nameOff` before resolving imports. */
typedef struct {
	nxaddr_t nameOff;    /* abs address of the needed library's NUL-terminated name */
} NxNeeded;

/* Padding (`_pad`/`_pN`) keeps the 8-byte address fields naturally aligned on x86_64 and
 * gives a deterministic layout that mknx and the loader must fill identically. On i686
 * nxaddr_t == uint32_t, so the layout differs from the historical v3 (the pads were added)
 * — that is fine: i686 mknx and the loader are recompiled from this same header (in-build
 * agreement, not compatibility with on-disk binaries, which we rebuild anyway). */
typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t flags;        /* NX_FLAG_* */
	uint32_t _pad;         /* keep entry 8-aligned on x86_64 */
	nxaddr_t entry;        /* abs entry address (.nxe only) */
	nxaddr_t loadBase;     /* preferred base the module was linked at */
	nxaddr_t imageSize;    /* bytes stored in the file (header + code + data + tables) */
	nxaddr_t bssStart;     /* abs; zeroed by the loader */
	nxaddr_t bssEnd;
	nxaddr_t importTable;  uint32_t importCount;  uint32_t _p0;   /* NxImport[] */
	nxaddr_t exportTable;  uint32_t exportCount;  uint32_t _p1;   /* NxExport[] */
	nxaddr_t relocTable;   uint32_t relocCount;   uint32_t _p2;   /* NxReloc[]  */
	nxaddr_t neededTable;  uint32_t neededCount;  uint32_t _p3;   /* NxNeeded[] */
} NxHeader;

#endif /* NXFORMAT_H */
