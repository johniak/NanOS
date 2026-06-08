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

#define NX_MAGIC   0x0045584E   /* 'N','X','E',0 little-endian */
#define NX_VERSION 2
#define NX_FLAG_DLL 1u          /* header.flags bit 0: module is a shared library */

/* An imported symbol: resolve `nameOff` against loaded modules' exports + kernel
 * exports, then store the address into the IAT slot at `slotAddr`. */
typedef struct {
	unsigned nameOff;    /* abs address of the import's NUL-terminated name */
	unsigned slotAddr;   /* abs address of the IAT slot (a function pointer) to patch */
} NxImport;

/* An exported symbol: `nameOff` is callable at address `addr` once the module is loaded
 * (and relocated). */
typedef struct {
	unsigned nameOff;    /* abs address of the export's NUL-terminated name */
	unsigned addr;       /* abs address of the exported symbol */
} NxExport;

/* A base relocation: the 32-bit word at `off` holds an absolute address; the loader adds
 * the load delta to it when the module loads at a non-preferred base. */
typedef struct {
	unsigned off;        /* abs address of the 32-bit word to fix up */
} NxReloc;

/* A needed shared library: load the .ndl named `nameOff` before resolving imports. */
typedef struct {
	unsigned nameOff;    /* abs address of the needed library's NUL-terminated name */
} NxNeeded;

typedef struct {
	unsigned magic;
	unsigned version;
	unsigned flags;        /* NX_FLAG_* */
	unsigned entry;        /* abs entry address (.nxe only) */
	unsigned loadBase;     /* preferred base the module was linked at */
	unsigned imageSize;    /* bytes stored in the file (header + code + data + tables) */
	unsigned bssStart;     /* abs; zeroed by the loader */
	unsigned bssEnd;
	unsigned importTable;  unsigned importCount;   /* NxImport[]  */
	unsigned exportTable;  unsigned exportCount;   /* NxExport[]  */
	unsigned relocTable;   unsigned relocCount;    /* NxReloc[]   */
	unsigned neededTable;  unsigned neededCount;   /* NxNeeded[]  */
} NxHeader;

#endif /* NXFORMAT_H */
