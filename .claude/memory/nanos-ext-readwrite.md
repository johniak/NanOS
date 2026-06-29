---
name: nanos-ext-readwrite
description: "ext2/ext4 gained full read+WRITE support (alloc, file write, dir ops, metadata syscalls, JBD2) — all 6 phases done"
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

NanOS ext2/ext4 went from read-only to full read+WRITE, no shortcuts, on branch `dockerized-build`. All six phases done, each gated by `e2fsck -fn` (clean) in the `nanos-build` container + host tests (≥90% coverage) + kernel build + `make check-arch`.

- **Faza 1** `950385c` — `fs/ext/ExtAllocator` (block/inode bitmaps, group+sb counts 64-bit, bitmap/desc/sb checksums, inode read/write).
- **Faza 2** `ba28cf9` — file write+truncate: `bmapAlloc`/`truncateBlocks` seams; ext2 direct+single/double/triple indirect, ext4 extent append + tree growth (arbitrary depth, extent-block tail csums) + split + collapse-to-inline.
- **Faza 3** `b4961ab`+`452500b`+`304f456` — dir ops in the shared core (linear, multi-block, dirent-tail csum): create/mkdir/unlink/rmdir/rename(incl cross-dir)/link/symlink + VFS + syscalls rmdir/rename/link/symlink.
- **Faza 4** `fde7601` — chmod/chown/utimes/statfs + syscalls (path + f* + the `*at` family + umask/creat/access/fsync).
- **Faza 5** `5afc3c7`+`922a54e` — JBD2: `Journal::replay` at mount (big-endian, v1 tags) + transactional writes (`txFlush`: writeTxn→checkpoint→resetLog). The ext4 fixture is journaled, so all ext4 tests run through the journal and stay e2fsck-clean.
- **Faza 6** `131606d` — ATA PIO write fixed (post-write BSY drain + FLUSH CACHE 0xE7 — was latent, never exercised); boot-time RW self-test writes `/disks/main/nanos/rwtest`. Verified under QEMU: boots to shell, post-session `e2fsck` clean, marker persisted, readable by Linux e2fsprogs.

Shared core in `fs/ExtFilesystem.h`; only `bmapAlloc`/`truncateBlocks`/`resolveBlock`/`initInodeBlockmap` differ per format (Ext2/Ext4Filesystem.h). Checksum primitives verified byte-exact vs the real ext4 image in `tests/test_extcsum.cpp`.

Cleanup pass (commits bdee400..f76668b) closed almost all the originally-deferred items: RamFs full parity (entries carry names -> hard links; symlinks; rmdir/rename/chmod/chown/utimes/truncate/statfs); utime/utimensat, true no-follow lchown, access/faccessat X_OK, renameat2 NOREPLACE/EXCHANGE; wall clock (Clock.h: RTC boot epoch + uptime) stamping mtime/ctime; 32-bit uid/gid (osd2 hi halves); process credentials uid/gid/euid/egid + getuid/setuid family + open() permission checks; non-extent ext4 now uses the shared direct/indirect map (de-duplicated into the core); JBD2 write barrier (flushExcept = log-before-data, data=ordered) + htree handled by de-indexing on insert (clear EXT4_INDEX_FL -> valid linear dir).

Genuinely NOT done (with justification): 64-bit block numbers beyond the 16 TiB ceiling (32-bit block nums already cover that; hi halves read as 0 — correct for any reachable size); JBD2 csum_v2/v3 / async-commit journals (mke2fs didn't enable them here; such a journal is declined, not mis-handled); ATA DMA (PIO is correct + proven; DMA is perf-only); scripted live power-cut (replay runs at every mount and is host-tested byte-exact, but a real crash isn't reproducible in CI). See [[nanos-multiprocessing-roadmap]].
