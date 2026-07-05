# Design: ext4 read-only support + shared ext core, VFS auto-detect

**Date:** 2026-06-06
**Status:** Approved

## Problem / Goal

NanOS reads ext2 read-only through the VFS. We want to add **ext4 read-only**
support and switch the system to use ext4 everywhere (the boot disk image becomes
ext4, mounted automatically), while keeping ext2 working. ext4 shares almost all
of its on-disk structures with ext2; the differences that matter for reading are
**extents** (an extent tree replaces direct/indirect block pointers) and **64-bit
block group descriptors**.

## Decisions (approved)

1. **Shared ext core**: factor common logic into an `ExtFilesystem` base; `Ext2Filesystem`
   and `Ext4Filesystem` specialise only the file-block-to-disk-block mapping.
2. **VFS auto-detect** via a `probe()` method on `FileSystemType`; mount type `"auto"`.
3. **Full extent tree** (depth 0 inline + deeper via index blocks) + 64-bit descriptors.

## Architecture

```
FileSystem (VFS interface)
  └── ExtFilesystem  (core: superblock, BGD, inodes, directories, read/stat/readdir,
        │             readFile, getInodeByPath; virtual resolveBlock())
        ├── Ext2Filesystem  resolveBlock(inode, i) = inode.directBlocks[i]
        └── Ext4Filesystem  resolveBlock(inode, i) = walk extent tree (fallback to
                            direct blocks when the inode lacks EXT4_EXTENTS_FL)
```

The seam is one virtual method:
`virtual unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex)`.
`readFile` and `getDirectoriesEntries` call it instead of indexing `directBlocks`
directly, so all shared machinery lives in `ExtFilesystem`.

### Refactor of the existing ext2 reader
`fs/Ext2Filesystem.h` currently holds all logic inline. Move the shared parts into
`fs/ExtFilesystem.h` (mount, initBgdt, getInode, getDirectoriesEntries,
getInodeByPath, getChildrenInode, isDirectory, readFile, read/stat/readdir).
`Ext2Filesystem` becomes a thin subclass implementing `resolveBlock` via direct
blocks (blocks 0-11; the existing read-only, direct-only behaviour). `ceil` moves to
`fs/ExtFilesystem.cpp`.

## ext4 specifics

- **Extents** (`fs/Ext4Filesystem.h`): the inode's 60-byte `i_block` area (the
  `directBlocks[12]` + indirect pointers) is reinterpreted as an `ext4_extent_header`
  followed by entries:
  - `ext4_extent_header { u16 magic=0xF30A; u16 entries; u16 max; u16 depth; u32 gen; }`
  - leaf `ext4_extent { u32 file_block; u16 len; u16 start_hi; u32 start_lo; }`
  - index `ext4_extent_idx { u32 file_block; u32 leaf_lo; u16 leaf_hi; u16 _; }`
  `resolveBlock` walks from the header: at depth>0 pick the index whose `file_block`
  covers the target and read that block as the next node; at depth 0 pick the extent
  covering the target and return `start_lo + (target - extent.file_block)`. Inodes
  without `EXT4_EXTENTS_FL` (0x80000) fall back to direct-block mapping.
- **64-bit descriptors**: read `s_desc_size` (superblock byte 0xFE) when the 64BIT
  incompat flag is set; otherwise 32. `initBgdt` strides the descriptor table by
  `descSize`, copying the first 32 bytes (low addresses) into the descriptor struct —
  correct for our <4 GB image.
- **feature flags**: `s_feature_incompat` at superblock byte 0x60. EXTENTS=0x40,
  64BIT=0x80. htree directories are read linearly (the linear entries remain valid);
  metadata checksums are ignored on read.

## VFS auto-detect (probe)

Add `virtual bool probe(BlockDevice* dev, unsigned partitionLba)` to `FileSystemType`.
`Vfs::mount(mountpoint, "auto", dev, lba)` iterates registered types and uses the
first whose `probe()` returns true.
- `Ext4FileSystemType::probe`: superblock magic 0xEF53 (byte 0x38) AND
  `s_feature_incompat & (EXTENTS|64BIT)`.
- `Ext2FileSystemType::probe`: magic 0xEF53 AND none of those flags.
Exactly one matches, so registration order is irrelevant. Explicit mount by name
("ext2"/"ext4") still works.

## "ext4 everywhere" + keeping ext2

- `scripts/create-image.sh`: `mke2fs -t ext2` → `mkfs.ext4`. GRUB2 reads ext4
  (extents) via its ext2 module — verified by booting in QEMU.
- `kernel/Kernel.cpp`: mount `"/"` with type `"auto"` (resolves to ext4).
- ext2 stays: `Ext2Filesystem` + `Ext2FileSystemType` still registered; the ext2
  fixture and tests are unchanged.

## Components / files
- New: `fs/ExtFilesystem.h` (core), `fs/ExtFilesystem.cpp` (`ceil`),
  `fs/Ext4Filesystem.h` (extents + `Ext4FileSystemType`)
- New tests: `tests/test_ext4.cpp`, `tests/fixtures/ext4.img`
- Modify: `fs/Ext2Filesystem.h` (thin subclass + `probe`), `fs/Vfs.h`/`fs/Vfs.cpp`
  (`probe` on `FileSystemType`, `"auto"` mount), `kernel/Kernel.cpp` (register ext4,
  mount "auto"), `Makefile` (SOURCES: `Ext2Filesystem.o` → `ExtFilesystem.o`;
  TEST_MODULES + COV_PATTERNS add ExtFilesystem/Ext4Filesystem)
- Reuse: `BlockDevice`, `RamBlockDevice` (fixture), `List`, `String`

## Testing (TDD, >90% coverage)
- New committed fixture `tests/fixtures/ext4.img` (mkfs.ext4), containing the same
  known files as the ext2 fixture **plus a file larger than one block** so extent
  spanning is exercised.
- `tests/test_ext4.cpp`: mount, readdir `/boot/grub` (contains grub.cfg), read exact
  contents of `/hello.txt`, read the multi-block file fully, stat type/size, missing
  path errors — all via `Ext4Filesystem` and via the VFS.
- Auto-detect tests: `Ext4FileSystemType::probe` true on the ext4 fixture / false on
  ext2, and vice-versa; `Vfs::mount("/", "auto", ...)` resolves correctly for both
  fixtures.
- Existing ext2 + VFS tests remain green; the shared `ExtFilesystem` core is covered
  by both suites. The lcov gate stays at ≥90% aggregate over the gated modules.

## Verification
1. `make test` — ext2, ext4, and auto-detect suites green; coverage ≥90%.
2. `make build` — kernel compiles (shared core + two subclasses) for i686.
3. `make run` (+ headless screendump) — kernel boots from the **ext4** GRUB2 image,
   auto-mounts "/", lists `/boot/grub`, prints `grub.cfg`. No triple fault.

## Risks
- GRUB2 must read the ext4 boot partition (extents) — verified in QEMU; if it cannot,
  fall back to an ext2 boot partition while still mounting ext4 data (revisit).
- Recent `mkfs.ext4` enables extra features (metadata_csum, flex_bg); these are
  read-compatible (checksums ignored, BGD still authoritative). Confirmed by tests
  against a real mkfs.ext4 fixture.
- Multi-level extent trees are hard to produce in a tiny fixture; the depth>0 walk is
  covered by logic tests where feasible and otherwise kept simple and reviewed.
