/*
 * NxFormat.h
 *
 * The NanOS binary format family (magic "NXE"). Same header + import-by-name
 * mechanism is shared by:
 *   .nxe   Nano Executable        (programs — implemented)
 *   .ndl   Nano Dynamic Library   (shared libraries — planned)
 *   .nkext Nano Kernel Extension  (loadable kernel modules — planned)
 *
 * Shared by the kernel loader and the user runtime. Plain C (no namespace) so
 * the user-side C files can include it too.
 *
 * Layout on disk (and in memory when loaded at header.loadBase):
 *   [ NxHeader | code | NxImport[] | data | (bss, not stored) ]
 * All addresses in the header/imports are ABSOLUTE (relative to loadBase).
 */
#ifndef NXFORMAT_H
#define NXFORMAT_H

#define NX_MAGIC 0x0045584E   /* 'N','X','E',0 little-endian */

typedef struct {
	unsigned nameOff;    /* abs address of the import's name string */
	unsigned slotAddr;   /* abs address of the IAT slot to patch */
} NxImport;

typedef struct {
	unsigned magic;
	unsigned version;
	unsigned entry;       /* abs entry address */
	unsigned loadBase;    /* where the image is linked/loaded (0x400000) */
	unsigned imageSize;   /* bytes stored in the file (header+code+data) */
	unsigned bssStart;    /* abs; zeroed by the loader */
	unsigned bssEnd;
	unsigned importTable; /* abs address of NxImport[importCount] */
	unsigned importCount;
} NxHeader;

#endif /* NXFORMAT_H */
