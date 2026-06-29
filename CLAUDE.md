# CLAUDE.md

Guidance for Claude Code (and humans) working in this repository.

> **This file is intentionally NOT committed.** It is a living, full-detail working
> document. Do not `git add` it.

---

## Project Overview

NanOS is a tiny x86 (i686, 32-bit protected mode) operating system written in C++
with assembly. Academic project exploring booting, interrupts, device I/O, and
filesystems. Not for production.

Current capabilities:
- Boots via **GRUB2** (Multiboot) from an **ext4** disk image.
- Sets up its own GDT/IDT, remaps the PIC, handles interrupts.
- A layered, Unix-style **storage stack**: VFS → filesystem (ext2/ext4) → block
  device HAL (ATA / RamDisk).
- Reads files read-only from ext2 **and** ext4 (auto-detected).
- A **Linux-style syscall interface** (`int 0x80`, real i386 numbers) over the VFS.
- A **custom executable format `.nxe`** + **Windows-style dynamic loader**: programs
  import system functions **by name**; the loader binds the Import Address Table to a
  kernel export table. Boots and runs `/bin/init.nxe` (ring 0) which cats `grub.cfg`
  through its imported API and exits.
- Multitasking exists but is experimental and **disabled**.

---

## Critical Workflow Facts (read first)

### Everything builds in Docker; only QEMU runs natively (macOS host)
The host is macOS; the cross toolchain and image tools live in Linux containers.
QEMU runs natively for speed.

- `make build` — compile the kernel in the `nanos-build` container → `bin/kernel.bin`.
- `make image` — build the GRUB2 ext4 disk image (`disk/image-grub2.img`) and write
  the kernel into it.
- `make run` — `make image` then boot natively: `qemu-system-i386 -drive file=disk/image-grub2.img,format=raw`.
- `make iso` / `make run-iso` — GRUB2 ISO via `grub-mkrescue`.
- `make test` — host-compiled doctest suite + coverage gate (native `nanos-test` image).
- `make coverage` — same + HTML report under `coverage/`.
- `make clean` — remove objects, image, iso, coverage.

The Makefile auto-detects host vs container via the marker file `/etc/nanos-build`
(baked into both images). Host targets shell into Docker; container targets
(`_all`, `_compile`, `_image`, `_iso`, `_test`, `_coverage`) do the real work.

### TWO Docker images
- `docker/Dockerfile` → `nanos-build`: Debian trixie-slim, **builds an `i686-elf`
  binutils+gcc cross-toolchain FROM SOURCE** (first build ~20–40 min, then cached),
  plus nasm, grub-pc-bin, grub-common, xorriso, mtools, e2fsprogs, parted, and lcov
  (lcov is in a trailing layer so it never invalidates the toolchain cache). Built
  for `linux/amd64` (emulated on Apple Silicon).
- `docker/Dockerfile.test` → `nanos-test`: lightweight **native-arch** image (g++,
  make, lcov). Tests need no cross toolchain or GRUB, so this avoids amd64 emulation
  and compiles the doctest suite in seconds.

### The macOS case-insensitivity trap (important)
The repo is bind-mounted from a **case-insensitive** macOS filesystem. With `-Ilib`
in the include path, `#include <string.h>` resolves to `lib/String.h` (the C++
`String` class) → infinite include recursion. Fix used everywhere: **compile from a
copy on the container's own case-sensitive filesystem**:
- kernel build copies to `/tmp/nanos-ksrc` (and copies `kernel.bin` back),
- test build copies to `/tmp/nbuild`.
Copies exclude `.git`, `disk`, `bin`, `iso`, `coverage` to stay fast.

Two separate include lists in the Makefile:
- `KINCLUDES` (kernel): all code dirs **plus `-Iinclude`** → `<string.h>` is the
  freestanding `include/string.h`.
- `HINCLUDES` (host tests): code dirs **without `-Iinclude`** → `<string.h>` is libc.

---

## Building the FULL system (every app + port)

`make image64` builds the **core system** from this repo alone. The **ports** (vim, bash,
git, openssl, netsurf, …) are separate `make <name>` targets that build from **sibling
repos** via the nanos-sdk, stage a `.nxe` into `bin/`, and are then folded into the image by
the next `make image64`. "Everything" = core + every port + assets, then re-image.
(All commands x86_64; the primary arch. `image64`/`run64` are inherently 64-bit.)

### Prerequisites (one-time)
- **Docker images** (auto-built on first use unless noted):
  - `nanos-build` (`docker/Dockerfile`) — kernel + in-tree userland + kexts + image.
    First build ~20–40 min (i686/x86_64 cross toolchain from source), then cached.
  - `nanos-test` (`docker/Dockerfile.test`) — host doctests (`make test64`).
  - `nanos-sdk-dev` — the **port build box**; lives in the *nanos-sdk* repo, required for
    every port (built there, not here).
  - `nanos-gltest` (`docker/Dockerfile.gltest`) — headless virgl QEMU for GPU-accel work.
- **Sibling repos (NOT in this repo; clone under `$HOME/Projects/`, override via the make var):**
  - `nanos-sdk` (`NANOS_SDK`) — universal `i686-nanos`/`x86_64-nanos` cross toolchain +
    `nanos-port` driver + the `nanos-sdk-dev` image.
  - `nanos-sdk-work` (`SDK_WORK`) — scratch where ports build (vim, grep, openssl, busybox,
    inetutils, wget, sqlite, bzip2, …).
  - `bash-nanos` (`BASH_FORK`) — the GNU bash 5.2 fork (`make bash`).
  - `netsurf-nanos` (`NETSURF_REPO`) — the graphical browser (`make netsurf`).

### 1. Core system — `make image64`
From this repo alone, into `disk/image64-grub2.img`:
- **kernel** `kernel.bin`; **kexts** `kbd mouse e1000 e1000e i219 virtio_gpu` (`.nkext`).
- **base userland** (`X64_USER_PROGS`): `init`, shell `nsh`, sbase coreutils (`cat ls mkdir
  rmdir pwd touch rm ln cp mv chmod wc head tail true false env basename dirname`), plus
  `free open nanosu chsh` and the stress/demo tools (`pfract pthrstress smptorture nettorture`).
- **desktop**: compositor `nwm`, login `greeter`, GUI apps `rsexp settings about notepad
  viewer properties form terminal`.
- **shared libs** (`.ndl`): `libc.ndl libnw.ndl libnwui.ndl`.

This alone boots to a working graphical desktop + shell.

### 2. Ports (x86_64-only; each builds via the SDK, stages into `bin/`)
Build the ones you want **in dependency order**, then re-run `make image64` to install them
(as `/apps/<name>` bundles + a `/bin` symlink). `make image64` never *depends* on a port — a
missing artifact is silently skipped.

- **Library prereqs first:** `make ARCH=x86_64 ncurses` (→ vim, htop);
  `make ARCH=x86_64 zlib libpng libjpeg` (→ netsurf).
- **Editors / tools:** `make ARCH=x86_64 vim`, `make ARCH=x86_64 htop`,
  `make ARCH=x86_64 grep`, `make ARCH=x86_64 sqlite`, `make ARCH=x86_64 bzip2`,
  `make toybox`, `make sudo`.
- **Shell / VCS:** `make bash` (from bash-nanos), `make git`.
- **Networking** (need `libc.ndl`): `make ping wget inetd httpd udhcpc` (+ inetutils
  telnet/telnetd/ifconfig/traceroute and darkhttpd via the SDK).
- **TLS / SSH:** `make openssl`, `make dropbear`.
- **Game:** `make ARCH=x86_64 doom`.
- **Browser:** `make netsurf` (needs `libnw`/`libnwui` + zlib/libpng/libjpeg).

Shortcut: `make externals` copies any **already-built** port artifacts into `bin/` in one
pass (best-effort; does NOT rebuild — use the per-port targets / nanos-sdk for that).

### 3. Assets — `make assets`
Converts `assets/wallpaper.png` + `logo.png` to NanOS surfaces in `bin/` (needs
python3 + Pillow). Installed under `/nanos/share` by `make image64`.

### 4. Re-assemble & run
After building ports/assets: `make image64` folds everything staged in `bin/` into the image.
Boot with `make run64` (native QEMU window) or the GPU-accel harness `scripts/run64-gl.sh`.

> Crossing arches reuses `bin/` for objects — run `make clean` when switching `ARCH`
> (the documented x86_64 convention) before building the other arch.

---

## Directory Structure (Linux-kernel style, MI/MD split)

The tree is split into **machine-independent (MI)** code and the **machine-dependent
(MD)** arch layer. MI code reaches hardware ONLY through the `<arch/...>` contracts in
`arch/include/arch/`; the x86 implementation lives under `arch/x86/`. Adding ARM later =
create `arch/arm/` implementing the same contracts (plus an `arch/arm/arch.mk`).

> This section describes the **source tree**. The **runtime/on-disk filesystem** — the VFS
> namespace (`/`, `/disks/main`, `/dev`, `/proc`, `/tmp`), the on-disk tree under
> `/disks/main` (the `/nanos` system subtree, `/apps` bundles, the `/bin` symlink farm) and
> why it is laid out that way, plus symlink/hardlink handling — is documented separately in
> @docs/en/filesystem.md (bilingual docs live under docs/en/ and docs/pl/; see docs/en/README.md)

```
arch/include/arch/  MI/MD CONTRACTS (included as <arch/foo.h>, on kernel + host-test paths):
           console.h (consolePutChar/Clear/SetCursor/Init), bootinfo.h (bootMemTop +
           bootMemForEachUsable), mmu.h (PAGE_* flags + mmuInitKernel; fwd-decl per-process
           AddressSpace API), irq.h (opaque TrapFrame + registerIrqHandler), cpu.h (cpuInit
           = GDT/IDT/keyboard; cpuDisableInterrupts/Enable/Halt), syscall.h (kernelSyscall +
           syscallInit/syscallSelfTest), block.h (bootDisk)
arch/x86/  MD implementation + per-arch build knobs:
   arch.mk   CROSS, ARCH_VPATH, ARCH_INCLUDES, ARCH_LINKER, ARCH_SOURCES
   linker.ld ENTRY(loader), .text@0x100000, .multiboot section first
   boot/     loader.s (Multiboot entry, GDT/IDT load stubs, mbd/magic), MultibootInfo.h,
             MultibootMmap (mmap parse), bootinfo_x86 (impl bootinfo.h)
   cpu/      Gdt, Idt, Interrupt(+Registers = x86 TrapFrame), IOPort, isr.S, irq.S,
             nxjmp.S, cpu_x86 (cpuInit/cli/sti), irq_x86 (impl irq.h), syscall_x86 (int
             0x80 decode -> kernelSyscall), MultiTasking (experimental, disabled, parked)
   mm/       Paging.h, PagingControl.h (CR0/CR2/CR3), AddressSpace (host-tested),
             mmu_x86 (impl mmu.h: identity-map + enable paging)
   drivers/  console_x86 (VGA sink), Keyboard (PS/2 IRQ1), Hdd + ATA.S (ATA PIO),
             AtaBlockDevice, block_x86 (impl block.h)
init/      kmain.cpp (MI entry called from loader)
kernel/    Kernel (start/idle loop, orchestrates via <arch/...>), Syscall (Syscalls core),
           SyscallDispatch (MI kernelSyscall switch), NxeLoader, Exec, NxFormat.h
drivers/   Console (MI formatting over arch sink), BlockDevice.h (HAL), DeviceManager,
           RamBlockDevice   — all MI now
fs/        Vfs (interfaces + manager), ExtFilesystem (shared core), Ext2/Ext4Filesystem
mm/        FrameAllocator (MI bitmap, host-tested), memory_manager (bump allocator)
lib/       String, List, icxxabi, string_funcs
include/   string.h (freestanding decls: memcpy/memset/strlen/strcmp/strstr)
disk/      disk images + legacy GRUB stages (images are gitignored build artifacts)
docker/    Dockerfile (build), Dockerfile.test (tests)
scripts/   create-grub2-image.sh (runs inside nanos-build; parted+mke2fs+grub-mkimage)
tests/     doctest.h, host_shims.cpp (stubs the arch console sink), test_*.cpp, fixtures/
docs/superpowers/  specs/ and plans/ (design + implementation docs)
Makefile, grub.cfg
```

Build: `ARCH ?= x86` selects `arch/$(ARCH)/arch.mk`, which supplies `CROSS`, `ARCH_VPATH`,
`ARCH_INCLUDES`, `ARCH_LINKER`, `ARCH_SOURCES`. The Makefile holds the portable `MI_SOURCES`;
`SOURCES = MI_SOURCES + ARCH_SOURCES`. `VPATH = init:kernel:drivers:fs:mm:lib:$(ARCH_VPATH)`;
includes stay `#include "Foo.h"` (resolved by `-I` dirs) and `#include <arch/foo.h>` for
contracts. **`make check-arch`** greps the MI dirs and fails if any x86 internal
(Registers / port I/O / inline asm / x86 headers) leaks back in — run it after MI edits.

---

## Architecture

### Boot
`arch/loader.s` (Multiboot header at load addr `0x100000`, set in `linker.ld`) →
`kmain()` (`init/kmain.cpp`) → `Kernel::start()` (`kernel/Kernel.cpp`).

`Kernel::start`:
1. `Gdt().initialize()` — **installs our own GDT** (null / 0x08 code / 0x10 data).
   This is mandatory: the IDT gates hardcode code selector `0x08`; without our GDT,
   GRUB2 hands off with the code segment elsewhere and the first hardware IRQ triple-
   faults. (This was the original "GRUB2 doesn't boot" bug.)
2. `Idt().initialize()` — remaps PIC to 0x20/0x28, sets 256 IDT gates, `sti`.
3. `Keyboard().initialize()`.
4. Storage stack wiring:
   ```cpp
   AtaBlockDevice* hd0 = new AtaBlockDevice("hd0");
   DeviceManager::registerDevice(hd0);
   Vfs* vfs = new Vfs();
   vfs->registerType(new Ext4FileSystemType());
   vfs->registerType(new Ext2FileSystemType());
   vfs->mount("/", "auto", hd0, 2048);   // partition at LBA 2048
   ```
5. Demo: `vfs->readdir("/boot/grub", ...)` + `vfs->read("/boot/grub/grub.cfg", ...)`.
6. Idle `while(1) loop()` (counter print disabled — quiet).

### Storage stack (the main subsystem)
```
FileSystem (VFS interface: mount/read/stat/readdir)
  └── ExtFilesystem  (shared ext2/3/4 core: superblock, group descriptors, inodes,
        │             directory parsing, getInodeByPath, readFile, read/stat/readdir)
        │   virtual resolveBlock(inode, fileBlockIndex) -> absolute fs block
        ├── Ext2Filesystem  resolveBlock = inode.directBlocks[i]  (direct blocks only)
        └── Ext4Filesystem  resolveBlock = walk extent tree (depth 0 inline + deeper
                            via index blocks); falls back to direct blocks if the
                            inode lacks EXT4_EXTENTS_FL (0x80000)

BlockDevice (HAL: readSectors/writeSectors/sectorSize/name)
  ├── AtaBlockDevice   wraps Hdd/ATA.S; issues ONE ATA command per sector
  └── RamBlockDevice   in-memory buffer (RamDisk driver AND test fixture)

DeviceManager   runtime registry of BlockDevice* (register/get/getByName)
Vfs             registry of FileSystemType* + mount table; path routing
                (longest-prefix mountpoint, boundary-aware); mount "auto" = probe()
```

- **Auto-detect:** `FileSystemType::probe(dev, lba)` reads the superblock; `Vfs::mount(
  ..., "auto", ...)` picks the first type whose probe matches. ext4 probe = magic
  0xEF53 + (EXTENTS|64BIT incompat flags); ext2 probe = magic + none of those. Exactly
  one matches.
- **64-bit descriptors:** `ExtFilesystem::mount` reads `s_desc_size` (superblock byte
  0xFE) when the 64BIT feature is set; `initBgdt` strides the descriptor table by it.
- ext4 features handled for read: extents, 64-bit, larger inodes (256). htree dirs are
  read linearly; metadata checksums ignored.

### Syscalls (Linux-style, int 0x80)
- `kernel/Syscall.{h,cpp}` — **host-testable** core: a `Syscalls` class with a
  file-descriptor table over a `Vfs*` + an injected console-write sink. fds 0/1/2 are
  the console; `open` allocates ≥3. Implements (Linux i386 numbers):
  `exit(1) read(3) write(4) open(5) close(6) lseek(19) fstat(108) getdents64(220)`.
  Returns negative errno (`-ENOENT/-EBADF/-EINVAL/-EROFS`). Writes to a file fd are
  `-EROFS` (FS is read-only).
- `kernel/SyscallDispatch.{h,cpp}` — **kernel-only** glue. `int 0x80` → IDT gate 128
  (DPL=3, set in `arch/Idt.cpp`) → `isr128` stub (`arch/isr.S`, uses `push dword`) →
  `isr_common_stub` → `syscallDispatch(Registers*)`, which reads nr from `eax`, args
  from `ebx/ecx/edx`, calls `Syscalls`, and writes the result to `regs->eax`. Return
  propagates to the caller because `isr_handler` takes `Registers` by value on the
  stack (so `popa` restores the modified `eax`). `installSyscalls(Vfs*)` registers it.
- No ring-3 userspace yet: `int 0x80` is currently issued from the kernel (CPL 0,
  allowed through the DPL=3 gate). Userspace (TSS, ELF loader, ring 3) is future work.

### Executables (.nxe) + dynamic loader (Windows-style import-by-name)
- Format `kernel/NxFormat.h` (shared, plain C): `NxHeader` (magic, entry, loadBase,
  imageSize, bss range, import table) + `NxImport` {nameOff, slotAddr}. Header sits
  first in the image; `objcopy -O binary` of the ELF yields the `.nxe` (no builder tool).
- `kernel/NxeLoader.{h,cpp}` — **host-testable** core: validates the header, zeroes
  bss, and binds the IAT by resolving each import name via a callback (bounds-checked).
- `user/` — the userland: `nx.ld` (base 0x800000, `.nxeheader` first; raised from 0x400000
  to give the kernel image low-memory headroom — staging+user window moved up in lockstep,
  see `kernel/Exec.cpp` STAGE_BASE + `arch/x86/mm/mmu_x86.cpp` + `usermode_x86.cpp`), `crt0.S`
  (`_start` → main → exit), `libnanos.{h,c}` (IAT + import descriptor + named wrappers
  `write/read/open/close/exit`), `nxhdr.c` (header from linker symbols), `init.c` (the
  program). Built by `make _userland` → `bin/init.nxe`, written to `/bin/init.nxe` in the
  image by `_image`.
- `kernel/Exec.{h,cpp}` — **kernel-only** glue: an export table (`write/read/open/close/
  exit` backed by the `Syscalls` core), `execProgram(vfs, path)` reads the `.nxe` to
  the staging window at 0x800000, calls `NxeLoader`, and runs it. `exit()` returns to the kernel via
  setjmp/longjmp (`arch/nxjmp.S`). The program runs on its own stack (top 0x500000).
- Stable, named API is the contract (one binary runs on any build); syscalls/internals
  live behind it. Fixed load base ⇒ single process, no relocation — multiprocess needs
  PIC/relocation/paging later.

### Adding a new filesystem or driver
- **Filesystem:** subclass `ExtFilesystem` (or `FileSystem` directly), implement
  `resolveBlock`; add a `FileSystemType` with `name`/`probe`/`create`; register it in
  `Kernel::start`. Add it to the test suite (see Testing).
- **Block driver:** implement `BlockDevice`; register an instance with
  `DeviceManager`. These runtime registries are the hook for future kext/module
  loading (not implemented).

### Other components
- `Console` (drivers): VGA text mode 0xB8000, 80x25, scrolling, cursor.
- `Keyboard`: IRQ1, scancode→ASCII, currently echoes to console (no input buffer yet).
- `Hdd`/`ATA.S`: ATA PIO ports 0x1F0-0x1F7. NOTE: `ata_lba_read` polls DRQ once and
  transfers `count*256` words — multi-sector reads desync the drive. `AtaBlockDevice`
  therefore issues **one command per sector**.
- `memory_manager`: bump allocator; `malloc/calloc/realloc` work, `free` is a no-op;
  overrides global `new`/`delete`. Declares `malloc` etc. with **C++ linkage** (not
  `extern "C"`) — host tests provide matching shims forwarding to libc builtins.
- `String`/`List`: project containers (no STL). `String` has an empty destructor (so
  shallow copies never double-free). `List` uses malloc/realloc/free consistently
  (fixed from a new[]/free mismatch).

---

## Key Constraints

- Freestanding C++: `-ffreestanding -nostdlib -nostdinc++ --no-exceptions --no-rtti
  -fno-leading-underscore`. **Virtual functions / abstract base classes DO work**
  (`--no-rtti` disables only RTTI, not vtables) — the whole VFS/HAL relies on this.
- **Global constructors are NOT run** (loader calls `kmain` directly, no `.ctors`).
  So never rely on a non-trivial global/static constructor. Patterns used instead:
  lazy init via a zeroed `.bss` pointer + `new` (DeviceManager), or construct objects
  with `new`/on the stack (Vfs, filesystems) so their member ctors run.
- `.bss` is zeroed by the multiboot loader, so zero-initialized globals are safe.
- No paging / single flat address space. No virtual memory.
- Two asm syntaxes: `.S` = NASM (Intel) — ATA.S, isr.S, irq.S; `.s` = GNU as (AT&T) —
  loader.s.
- Memory map: kernel loaded at `0x100000` (1 MiB); bump-allocator heap starts high
  (placeholder address ~117 MB) — fine within QEMU's default 128 MB RAM.

---

## Testing (TDD, host-compiled, >90% coverage gate)

- Framework: **doctest** (`tests/doctest.h`, vendored). Real `TEST_CASE`/`CHECK`.
- The storage stack is pure software, so it runs **natively on the host** via
  `RamBlockDevice` — no QEMU needed for unit/integration tests.
- `tests/host_shims.cpp`: host stand-ins for `Console` and the C++-linkage allocator
  symbols (forward to `__builtin_malloc` etc.). Memory/string come from libc by NOT
  linking `memory_manager`/`string_funcs` and NOT using `-Iinclude`.
- Fixtures: committed `tests/fixtures/ext2.img` and `ext4.img` (raw filesystems with
  known files; ext4 includes a multi-block file to exercise extents). Tests load them
  into a `RamBlockDevice`.
- Coverage: modules under test compiled with `--coverage`; `lcov` extracts the gated
  modules (`COV_PATTERNS` in the Makefile) and **fails `make test` if line coverage
  < 90%** (`COV_MIN`). Current: 36 tests, ~96% aggregate.
- `TEST_MODULES` / `COV_PATTERNS` in the Makefile list what's compiled/gated. Add new
  modules there. `AtaBlockDevice` is excluded (touches hardware) — verified via QEMU.
- **Test gotchas:** registered devices/types must have static/heap lifetime (the
  registries keep pointers — stack objects dangle across test cases). Don't
  `#include <cstdlib>` in tests (its C-linkage `malloc` clashes with the C++-linkage
  decl from `memory_manager.h`); use libc via the shims.

### Verifying a kernel change in QEMU (headless)
The pattern used throughout: build the image with `grub.cfg` `timeout=0`, boot with
`-display none -monitor unix:/tmp/qmon,server,nowait`, then via a Python socket to the
monitor send `screendump /tmp/x.ppm`, convert with `sips -s format png`, and read the
PNG. Restore `grub.cfg` (timeout=5) afterward. To diagnose faults: add
`-no-reboot -d int -D log`, then grep for `v=08`/`v=0d` (triple/GP fault).

---

## Conventions

- **Commits/PRs must contain NO mention of Claude** — no `Co-Authored-By`, no
  "Generated with" trailer. Clean messages only.
- TDD: write the failing test, watch it fail, minimal code to pass, refactor, commit.
- Work on a branch, not `master`. Current feature branch: `dockerized-build`
  (Docker build → GDT fix → storage stack → directory restructure → ext4), 17 commits
  ahead of `origin/master`, not pushed.
- Design/implementation docs live in `docs/superpowers/specs/` and `plans/`.

---

## Bugs Found & Fixed This Cycle (context)

These were latent and surfaced by getting the read path actually exercised:
1. **No own GDT** → IDT gates' code selector invalid under GRUB2 → triple fault on
   first IRQ. Fixed: install a flat GDT before `sti`.
2. **ext2 use-after-free**: `getInodeByPath`/`getChildrenInode` returned `&local`.
   Fixed: return by value via out-param.
3. **Hardcoded 128-byte inode size** (modern mke2fs uses 256). Fixed: read from
   superblock.
4. **Extended superblock parsed from offset 0** instead of after the 84-byte base →
   garbage inode size. Fixed: offset by `sizeof(base)`.
5. **`List` new[] + realloc/free mismatch** (+ double free in growth). Fixed: malloc
   family throughout.
6. **ATA `rep insw` multi-sector desync** → reads returned zeros. Fixed: one ATA
   command per sector in `AtaBlockDevice`.
7. **`readFile` multi-block math** was wrong. Fixed: clean block-by-block loop via
   `resolveBlock`.

---

## Designed but NOT built (future)

- **shell** (keyboard input buffer + blocking read + `ls`/`cat` as `.nxe` programs).
- **ring-3 userspace**: TSS, user-mode segments, real privilege separation (programs
  currently run in ring 0; the `.nxe` loader + syscalls are ready for ring 3).
- **multiprocessing**: a scheduler switching between several loaded `.nxe` programs
  (builds on the single-process loader; needs per-process state + PIC/relocation/paging).
- **kext / dynamic driver loading** — the `.nxe` import/export mechanism is the natural
  basis; `DeviceManager` / `Vfs::registerType` are the registration hooks.
- **kext / dynamic module loading** — the runtime registries (`DeviceManager`,
  `Vfs::registerType`) are the intended hook.
- ext2/ext4 **write** support; ext2 indirect blocks; real deep (depth>1) extent trees
  beyond the crafted unit test; other filesystems (ramfs/devfs).
