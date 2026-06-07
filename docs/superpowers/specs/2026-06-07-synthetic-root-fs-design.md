# NanOS — Synthetic root filesystem (`/` virtual, disks under `/disks/`)

_Design spec. Date: 2026-06-07. Branch: `dockerized-build`._

## Goal

Replace the current "mount the ext disk directly at `/`" layout with a **deliberately
non-Unix** namespace:

- **`/` is a synthetic, in-memory filesystem** (not a physical volume).
- It hosts virtual subtrees: **`/disks`** (mounted physical volumes), **`/dev`**
  (device nodes), **`/proc`** (dynamic info), and arbitrary nodes added later.
- **Physical volumes mount under `/disks/<name>`** — the system disk at
  **`/disks/main`** by default (name configurable; `main` is a fixed default for now).

Paths shift from `/boot/grub/grub.cfg` to `/disks/main/boot/grub/grub.cfg`. Hierarchical
paths and the path-based VFS are unchanged — only the volume layout is non-Unix
(closer to Plan 9 / `/Volumes` than to Unix's single root mount).

## Non-goals (v1)

- Writable synthetic nodes — `/dev` is read-only; `/dev/null` simply returns EOF on
  read (no write path; `write` to a non-console fd stays `-EROFS`).
- Volume labels / auto-naming — the boot volume is the fixed string `"main"`.
- A full procfs — one demo dynamic node only.
- Removing/rethinking the path-based `FileSystem` interface (kept as-is).

## Architecture

```
Syscalls (fd table: path + offset)
   └─ Vfs (longest-prefix mount routing — UNCHANGED)
        ├─ "/"            -> SynthFs   (NEW: in-memory virtual tree)
        └─ "/disks/main"  -> ExtFilesystem (ext2/4, the physical disk)
```

`SynthFs` plugs into the existing `Vfs` mount table; routing already does
longest-prefix matching, so:
- `read("/disks/main/boot/x")` → `/disks/main` (ext) → relative `/boot/x`.
- `readdir("/disks")` → `/` (SynthFs) → lists volume names under its `/disks` node.
- `read("/dev/random")` → `/` (SynthFs) → relative `/dev/random`.
- `stat("/disks/main")` → `/disks/main` (ext) → ext root dir.

### `SynthFs` — an in-memory tree (new `FileSystem`)

A tree of `SynthNode`s walked by path. Three node kinds:

```cpp
enum SynthKind { SYNTH_DIR, SYNTH_STATIC, SYNTH_GEN };

struct SynthNode {
    char name[64];
    SynthKind kind;
    // SYNTH_DIR:    children (List<SynthNode*>)
    // SYNTH_STATIC: const char* data; unsigned len;
    // SYNTH_GEN:    int (*read)(unsigned offset, void* buf, unsigned n);  // device/proc
};
```

`SynthFs` implements the `FileSystem` interface over this tree:
- `read(path,size,off,buf)` — walk to node; STATIC copies from `data`; GEN calls the
  generator; DIR → error.
- `stat(path,out)` — DIR → `NODE_DIR`; STATIC → `NODE_FILE`, `size = len`; GEN →
  `NODE_FILE`, `size = 0`. Synthetic mode bits (e.g. `0555` dir, `0444` file,
  `0666`/char for `/dev`), `nlink = 1`, `uid/gid = 0`, `mtime = 0`.
- `readdir(path,List)` — DIR → list children (name + type).

Construction (no global ctors, so build with `new` in `Kernel::start`):
- root `/` (DIR) with children: `disks` (DIR), `dev` (DIR), `proc` (DIR).
- `/dev`: `null` (GEN: read→0/EOF), `zero` (GEN: fill `buf` with 0, return `n`),
  `random` (GEN: fill with a simple LCG/xorshift pseudo-random stream).
- `/proc`: `uptime` (GEN: render a counter as text). One node for v1.
- `addVolume(name)` adds a placeholder DIR child under `/disks` so `readdir("/disks")`
  lists it (content is served by the ext mount, not this node).

### Mounting volumes — `mountVolume()` helper

Single source of truth via a small helper (kernel-side glue):

```cpp
void mountVolume(Vfs* vfs, SynthFs* root, const char* name,
                 BlockDevice* dev, unsigned lba) {
    char mp[80]; /* "/disks/" + name */
    vfs->mount(String(mp), "auto", dev, lba);   // routing of content
    root->addVolume(name);                       // visibility in readdir("/disks")
}
```

### Vfs change — mount a pre-built FileSystem

`SynthFs` has no `BlockDevice`, so the current
`Vfs::mount(mountpoint, fstype, dev, lba)` (which goes through `FileSystemType::create`)
doesn't fit. Add a direct overload:

```cpp
int Vfs::mount(String mountpoint, FileSystem* fs);   // mount an already-built FS
```

The existing type/device `mount` stays for disks. Internally both add a `Mount{mp, fs}`.

### Boot wiring (`Kernel::start`)

```cpp
SynthFs* root = new SynthFs();          // builds /, /disks, /dev, /proc
Vfs* vfs = new Vfs();
vfs->registerType(new Ext4FileSystemType());
vfs->registerType(new Ext2FileSystemType());
vfs->mount("/", root);                  // synthetic root
mountVolume(vfs, root, "main", hd0, 2048);
```

### Path updates (callers)

- `nsh`: bare `ls` → `ls /` now lists `[disks, dev, proc]` (fine). Drop the old
  `ls -> ls /` special-case? No — keep `ls` (no arg) → `ls /`.
- Any demo/QEMU paths: `/boot/...` → `/disks/main/boot/...`.

## Data flow examples

- `ls /` → SynthFs readdir → `disks  dev  proc`.
- `ls /disks` → SynthFs readdir of `/disks` → `main`.
- `ls -l /disks/main/boot` → ext (real metadata, as today).
- `cat /disks/main/boot/grub/grub.cfg` → ext read.
- `cat /dev/zero` → SynthFs GEN streams zeros (cat loops; bounded by the user piping
  or Ctrl-C — for the demo we just confirm it runs).
- `cat /proc/uptime` → SynthFs GEN renders the uptime text.

## Error handling

- Walk to a missing path → `-1`/`-ENOENT` (same convention as ext).
- `read` on a DIR → error; `readdir` on a non-DIR → error (mirrors ExtFilesystem).
- GEN read past logical end (static) → 0 (EOF); device gens decide their own EOF
  (`null` → 0 immediately; `zero`/`random` → always return `n`).

## Testing (host doctest, ≥90% gate)

`SynthFs` is pure in-memory software → fully host-testable. New `tests/test_synthfs.cpp`:
- tree readdir: `readdir("/")` lists disks/dev/proc; `readdir("/dev")` lists null/zero/random.
- stat: `/` and `/dev` are dirs; `/proc/uptime` is a file.
- static read: a static node returns its bytes; offset/partial reads.
- generator read: `/dev/zero` returns N zero bytes; `/dev/null` returns 0; `/dev/random`
  returns N bytes (and two reads differ).
- `addVolume("main")` then `readdir("/disks")` lists `main`.
- missing path → error.

Add `fs/SynthFs.cpp` to `TEST_MODULES` + `COV_PATTERNS`. The `mountVolume` helper and
boot wiring are exercised in QEMU.

QEMU (headless screendump): boot; `ls /`, `ls /disks`, `ls -l /disks/main/boot`,
`cat /disks/main/boot/grub/grub.cfg`, `cat /proc/uptime`. No faults.

## Files

- Create: `fs/SynthFs.h`, `fs/SynthFs.cpp` (the synthetic FS + node model).
- Modify: `fs/Vfs.h`, `fs/Vfs.cpp` (add `mount(mountpoint, FileSystem*)` overload).
- Modify: `kernel/Kernel.cpp` (boot wiring; `mountVolume` helper, or put it in Exec/Kernel).
- Modify: `user/nsh.c` if any default path assumes the old layout (bare `ls` stays `/`).
- Modify: `Makefile` (`MI_SOURCES += SynthFs.o`; `TEST_MODULES`/`COV_PATTERNS`).
- Create: `tests/test_synthfs.cpp`.

## Build order (one commit each; build + test + check-arch green)

1. `SynthFs` core + node model (dir/static/gen) + host tests (readdir/stat/read/gen).
2. `Vfs::mount(mountpoint, FileSystem*)` overload + host test.
3. Wire boot: SynthFs at `/`, `mountVolume("main", hd0)`; update paths; QEMU verify
   `ls /`, `ls /disks`, `ls -l /disks/main/boot`, `cat /disks/main/boot/grub/grub.cfg`.
4. `/dev/{null,zero,random}` + `/proc/uptime` generators; QEMU `cat /dev/...`, `cat /proc/uptime`.
```
