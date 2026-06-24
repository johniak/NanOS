# NanOS Filesystem Layout

Two distinct things share the word "filesystem" here, and it helps to keep them apart:

1. **The VFS namespace** — the unified path tree a running program sees (`/`, `/disks`,
   `/dev`, `/proc`, `/etc`, `/tmp`, …). It is assembled at boot from several filesystem drivers
   mounted at different points. Nothing is "mounted at `/`" in the Unix sense of a disk —
   the root is synthetic.
2. **The on-disk layout** — the directory tree actually stored on the ext2/ext4 disk
   image, which is mounted into the namespace under `/disks/main`.

---

## 1. The VFS namespace (runtime)

Assembled in `kernel/Kernel.cpp` (`Kernel::start`). All access goes through the VFS
(`fs/Vfs.*`), which routes a path to the right driver by longest-prefix mountpoint match.

```
/                     SynthFs  — synthetic in-memory root (no disk). Holds the mount
│                              points + /proc + /dev. Read-only, generated on the fly.
├── disks/
│   └── main/         ext2/ext4 (auto-detected) — THE physical disk image, partition
│                     discovered from the MBR. READ-WRITE: writes persist to disk and
│                     survive reboot (see §2 + §5). Everything persistent lives here.
├── dev/
│   ├── fb0           CharDevice — the firmware framebuffer (fbdev ioctls + mmap +
│   │                 read/write). Present only if the bootloader provided a framebuffer.
│   ├── input0        CharDevice — the keyboard as a Linux-style evdev: the PS/2 IRQ feeds
│   │                 scancodes, programs read() 2-byte key down/up events.
│   ├── tty1..tty7   CharDevice — the virtual terminals (Ctrl+Alt+Fn). tty1–6 are kernel fbcon
│   │                 text consoles (a login each); tty7 is the graphics console (nwm). See x86_64.md §5.3.
│   ├── tty0          CharDevice — the active VT; tty (no number) = the caller's controlling VT.
│   ├── console       CharDevice — the kernel console (= tty1; boot messages + panics).
│   ├── ptmx         CharDevice — PTY master, held by the userspace terminal emulator.
│   ├── pts0         CharDevice — PTY slave, the shell's controlling tty.
│   └── tty          CharDevice — the controlling terminal (a VT via Process::cttyVt, else the pty).
├── proc/             SynthFs — synthetic, Linux-style. Generated per read:
│   ├── meminfo       MemTotal/MemFree (physical RAM) + KHeapTotal/KHeapFree (kernel heap).
│   ├── uptime        seconds since boot (advances via the scheduler clock).
│   ├── version       kernel identification string.
│   └── <pid>/…       one entry per process + kernel thread (e.g. [idle]).
├── etc/              RamFs — a writable in-memory tmpfs, populated at boot from the on-disk
│                     template /disks/main/nanos/config/etc/. Linux apps find resolv.conf,
│                     hosts, nsswitch.conf, protocols, services here (DHCP rewrites
│                     resolv.conf at runtime). Lives in tmpfs so runtime edits don't touch
│                     the disk image; cleared/regenerated on reboot.
└── tmp/              RamFs — a writable in-memory tmpfs. Cleared on reboot. Programs put
                      transient files here (e.g. Doom's config + savegames).
```

**Why a synthetic root instead of mounting the disk at `/`?** It keeps the machine-visible
namespace (devices, process info, scratch space) independent of any one disk, and lets
multiple volumes mount side by side under `/disks/<name>` — the macOS/Plan 9 style rather
than the "one disk is the root" Unix style. The disk is just one citizen under `/disks`.

**Writability:** the ext driver (`fs/ExtFilesystem.*`) is now **fully read-write** — writes
to `/disks/main` allocate blocks/inodes, update metadata, and **persist to disk across
reboot**, journalled through JBD2 (see §5). The in-memory `/tmp` and `/etc` (RamFs) are also
writable but transient (cleared on reboot). Only `/` and `/proc` are read-only — they are
generated on the fly, not stored anywhere.

---

## 2. The on-disk layout (`/disks/main`)

The ext2/ext4 image (`disk/image-grub2.img`, built by `scripts/create-grub2-image.sh`,
populated by the `_image` target in the Makefile). Laid out so that **the OS, user apps,
and the run-by-name PATH are cleanly separated**:

```
/disks/main/
├── boot/grub/                 GRUB2 stages + grub.cfg (the bootloader; not touched at runtime).
│
├── nanos/                     EVERYTHING that IS the operating system lives here.
│   ├── core/
│   │   ├── kernel.bin         the kernel (loaded by GRUB via multiboot).
│   │   └── init.nxe           PID 1 — the first user program; execve()s into the shell.
│   ├── bin/                   SYSTEM utilities (flat, no bundle): nsh, ls, cat, free + the basic
│   │                          coreutils (mkdir, rmdir, rm, touch, mv, cp, ln, pwd, chmod, wc,
│   │                          head, tail, true, false, env, basename, dirname) — in-tree sbase.
│   ├── lib/                   SHARED LIBRARIES (.ndl): libc.ndl, greet.ndl. The dynamic
│   │                          loader (kernel/DynLoader.cpp) resolves "needed" libraries
│   │                          here, by name, at exec time.
│   ├── kext/                  loadable kernel modules (.nkext): PS/2 keyboard + mouse, e1000
│   │                          NIC. Loaded at boot by loadAllKexts() — NOT in the kernel image.
│   │                          See kext.md.
│   ├── config/                system config (NanOS's /etc). Holds `passwd` — the account
│   │                          database; its 7th field is the login shell, so editing it sets
│   │                          the default shell (init/nterm launch getpwuid()->pw_shell).
│   │                          `config/etc/` is the template copied into the writable /etc
│   │                          tmpfs at boot (resolv.conf, hosts, nsswitch.conf, …).
│   ├── cache/ logs/           reserved: caches / logs (future).
│
├── apps/                      NON-SYSTEM apps, each a self-contained BUNDLE directory:
│   └── <name>/
│       ├── <name>.nxe         the app's entry binary.
│       └── …                  the app's own data files (e.g. apps/doom/doom1.wad).
│
└── bin/                       LINK FARM: a flat directory of symlinks
    └── <name>.nxe  ->  /apps/<name>/<name>.nxe   (a symlink, NOT a copy)
```

### Why this split

- **`/nanos` = the OS, one self-contained subtree.** Core (kernel + init), system
  utilities, shared libs, and future system data all live under one root, so "the OS" is a
  single movable/identifiable thing (NeXT/macOS `/System` flavour).
- **`/nanos/bin` (system) vs `/apps` (everything else).** `ls`/`cat`/`free`/`nsh` are part
  of the system and sit flat in `bin`. A game or demo (`doom`, `usedll`) or a test tool is
  **not** the system — it lives in its own bundle directory `apps/<name>/` together with
  its data. Removing or adding an app is just adding/removing one directory.
- **App bundles** keep a program and its assets together (à la macOS `.app` / a game
  folder). `doom` opens its IWAD by absolute path `/disks/main/apps/doom/doom1.wad` — the
  binary and the data ship as a unit.
- **`/bin` is a link farm**, the way the shell runs apps *by name* without knowing the
  bundle layout (the `/usr/local/bin` → Homebrew Cellar pattern). Each entry is a **symbolic
  link** into the bundle, so there is no second copy of the binary; delete the bundle and
  the link dangles.

### How a command is resolved (`user/nsh.c`)

For a bare command name, the shell tries, in order:

1. `/disks/main/nanos/bin/<cmd>.nxe`   — a system utility,
2. `/disks/main/bin/<cmd>.nxe`         — an app via the link farm (symlink → bundle),
3. `/disks/main/apps/<cmd>/<cmd>.nxe`  — the bundle directly (fallback).

A name starting with `/` is run as an explicit path. Shared libraries an executable
`needs` are always resolved from `/disks/main/nanos/lib/<name>.ndl` regardless of where the
program itself lives.

---

## 3. Links

The ext driver (`fs/ExtFilesystem.h`) resolves both kinds of link during path lookup, and
can also **create** them (`symlink()`, `link()`):

- **Symbolic links** — `getInodeByPath` follows a symlink component to its (absolute)
  target, with a depth guard against loops. Short targets are read inline from the inode's
  60-byte `i_block` area (ext "fast symlink"); long ones from a data block ("slow
  symlink"). Used for the `/bin` link farm.
- **Hard links** — work transparently: a hard link is just a second directory entry
  pointing at the same inode number, so resolution finds the same file with no special
  code. (Verified in `tests/test_ext2.cpp`.)

---

## 4. The storage stack (how a path read/write reaches the disk)

```
VFS (fs/Vfs)              path routing: longest-prefix mountpoint -> a FileSystem driver
  ├── SynthFs             generated trees: / and /proc and /dev nodes
  ├── RamFs               in-memory read/write tmpfs (/tmp, /etc)
  └── ExtFilesystem       ext2/ext4 read+write core (superblock, inodes, dirs, symlinks)
        │                   helpers under fs/ext/:
        │                     BlockCache    write-back block cache (dirty tracking + flush)
        │                     ExtAllocator  block/inode bitmap alloc/free (+ csums)
        │                     Journal       JBD2 transaction log (replay/write/reset)
        ├── Ext2Filesystem  resolveBlock = direct (+ indirect) blocks
        └── Ext4Filesystem  resolveBlock = extent tree

BlockDevice (drivers/BlockDevice.h)   HAL: readSectors/writeSectors/sectorSize
  ├── AtaBlockDevice     real disk (ATA PIO), one command per sector (read AND write)
  └── RamBlockDevice     in-memory buffer (and the test fixtures)

DeviceManager            runtime registry of block devices
```

A **read** of `/disks/main/apps/doom/doom.nxe` → VFS matches the `/disks/main` mount → the
ext driver resolves the path (following the symlink if it came in via `/bin`) →
`resolveBlock` maps file offsets to disk blocks → `AtaBlockDevice` issues the ATA reads.

A **write** to `/disks/main/...` → the ext driver allocates blocks/inodes via `ExtAllocator`,
updates the inode/dir/bitmap blocks through `BlockCache`, and on each operation calls
`txFlush()`, which journals the dirty metadata as a JBD2 transaction (log → checkpoint →
reset) before letting `AtaBlockDevice` write it to the physical disk (see §5).

---

## 5. Write support & journaling

The ext driver is a full read+write filesystem for both ext2 and ext4:

- **File data:** `write()` (with block allocation), `truncate()` (grow/shrink, freeing or
  allocating blocks), `create()` (make-or-truncate a regular file).
- **Directories:** `mkdir()`, `rmdir()`, `unlink()`, `rename()`, plus `link()` / `symlink()`.
- **Metadata:** `chmod()`, `chown()` / `lchown()`, `utimes()`.
- **Allocation** (`fs/ext/ExtAllocator.*`): per-group block and inode bitmaps, live free
  counts kept in sync in the superblock, and recomputed bitmap / group-descriptor checksums
  for ext4 `metadata_csum`.
- **Block cache** (`fs/ext/BlockCache.*`): a write-back cache that stages dirty blocks
  (`write` / `writePartial` for read-modify-write) and flushes them to the device; `Journal`
  drives a selective flush (log blocks first, then data).
- **Journaling** (`fs/ext/Journal.*`, JBD2): on mount the driver runs `recoverJournal()` —
  it replays any committed transactions left by an unclean shutdown and clears the
  `INCOMPAT_RECOVER` flag. Every mutating operation is wrapped by `txFlush()`, which writes
  the changed metadata to the journal, checkpoints it to its final location, then resets the
  log — so the on-disk image stays consistent.
- **Verified at boot:** `extRwSelftest()` in `kernel/Kernel.cpp` writes a marker file to
  `/disks/main/nanos/rwtest`, reads it back, and reports whether last boot's marker
  survived — exercising the whole VFS → ext write + JBD2 → ATA write → disk path. The image
  is **e2fsck-clean** after these writes.
