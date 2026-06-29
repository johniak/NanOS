---
name: nanos-coreutils
description: Basic coreutils (mkdir/rm/touch/cp/mv/ln/chmod/...) shipped as in-tree sbase ports; fixed several latent kernel/libc bugs
metadata: 
  node_type: memory
  type: project
  originSessionId: 661d85b4-63ba-4deb-8adc-c850a1d3125e
---

The basic file/dir coreutils are shipped on NanOS, closing the `bash: touch: command not found`
gap from the [[nanos-git-port]]. Built as **in-tree sbase ports** (verbatim upstream @ commit
`c546c3a`, sources under `user/third_party/sbase/`, compiled like `cat`/`ls`, dynamically linked
against libc.ndl, installed to `/nanos/bin` via `SYS_PROGS`). Plan + per-task log:
`docs/superpowers/plans/2026-06-16-coreutils.md` (executed via a ralph loop, one task per commit).

**Shipped — tier 1:** mkdir, rmdir, rm (-r), touch, mv, cp (-r), ln (-s), pwd.
**tier 2:** chmod, wc, head, tail, true, false, env, basename, dirname. (`stat` skipped — sbase has
none; `ls -l` covers it.)

**Latent kernel/libc bugs fixed along the way** (all in the NanOS repo, surfaced by exercising the
tools — reusable, not coreutils-only):
- `chmod`/`chown`/`fchmod`/`fchown`/`lchown` were no-op stubs in `posixstubs.c` (read-only-era
  relic) → now real syscalls (moved to `syscalls.c`).
- The whole `*at` family + `utimes`/`utimensat`/`futimens` libc wrappers were missing → added in
  `syscalls.c`. `futimens` = `utimensat(fd, "", t, 0)` + a kernel `resolveAt` AT_EMPTY_PATH branch.
- **`*at` constant ABI mismatch:** picolibc (newlib) uses different AT_FDCWD/AT_REMOVEDIR/
  AT_SYMLINK_NOFOLLOW values than Linux (which the kernel + SDK ports use). The kernel now accepts
  BOTH encodings (`kernel/Syscall.cpp` IS_AT_FDCWD/HAS_AT_REMOVEDIR/HAS_AT_NOFOLLOW) — the value
  sets don't collide.
- **`st_ino` was hardcoded to 1 for every file** → real inodes now (`FileStat.ino`, ext via
  `resolvePathNum`, SynthFs/RamFs via node pointer, kernel `fillStat`). Fixes rm's root-protection
  AND hard-link identity. No ABI change (knl_stat already had the ino field).
- `mkdir -p` aborted on read-only parents → SynthFs::mkdir + Vfs::mkdir report EEXIST before
  EROFS/ENOENT (POSIX ordering); host tests added.
- libc `rename` was a copy+unlink stopgap → now real `SYS_rename`, so `mv` moves directories and
  preserves inode/hard-links.

**compat-decls.h additions** (picolibc gaps): UTIME_NOW/OMIT (Linux tv_nsec encoding), strptime
proto, mknod proto. One minimal documented patch to vendored `touch.c` (Zulu/`tm_gmtoff` — NanOS is
UTC-only and picolibc's struct tm lacks the field). `creat` (SYS_creat) + a `mknod` ENOSYS stub
added to libc.

Limitations (by design): no `realpath` (`pwd -P`/`stat` lean on getcwd/`ls -l`); `cp -a` of
device/fifo nodes unsupported (no mknod syscall); `touch -d …Z` = UTC.
