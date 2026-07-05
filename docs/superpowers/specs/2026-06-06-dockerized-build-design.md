# Design: Fully Dockerized Build, Native QEMU

**Date:** 2026-06-06
**Status:** Approved

## Problem

The GRUB2 boot migration is incomplete. Investigation (headless QEMU screendumps)
established:

- The Docker-built GRUB2 HDD image is valid: GRUB2 installs correctly and shows the
  `NanOS` menu entry.
- The kernel is currently compiled with the **native macOS Homebrew** `i686-elf`
  toolchain, not in Docker.
- The kernel does not visibly boot through GRUB (the multiboot handoff does not
  complete).

The most likely culprit is the inconsistent, host-specific toolchain. The goal is to
move **all build steps into a Linux Docker container** for reproducibility, leaving
only QEMU running natively on macOS for testing.

## Goals

- All compilation, linking, and image creation happen inside a single Docker container.
- The host (macOS) needs only `docker` and `qemu`.
- A transparent wrapper: `make run` builds in Docker, then launches native QEMU.
- Two boot paths kept: **HDD GRUB2** (primary) and **ISO** (quick dev). The legacy
  floppy / `fs/image.img` path is removed.
- Toolchain: build `i686-elf` binutils + gcc **from source** in the Docker image
  (maximum fidelity to the intended bare-metal target).

## Non-Goals

- Fixing kernel logic bugs beyond the build/boot pipeline. If the kernel still fails
  to boot after the Dockerized rebuild, that is a separate systematic-debugging task on
  the multiboot handoff (see Verification).
- Write support or any kernel feature work.
- Supporting non-Docker native builds on macOS going forward.

## Architecture

### Host / container split in one Makefile

A single `Makefile` detects whether it runs inside the build container via a marker
file (`/etc/nanos-build`) baked into the image.

```
HOST side (macOS, no marker)             CONTAINER side (Linux, marker present)
────────────────────────────            ──────────────────────────────────────
docker-image: docker build docker/       _all:        compile + link + grub2-image
run:     docker run → make _all          compile/link: i686-elf-gcc / -as / ld
         then qemu (native)              grub2-image:  parted + mke2fs + grub-mkimage
run-iso: docker run → make iso           iso:          grub-mkrescue
         then qemu (native)              (toolchain resolved from container PATH)
```

The `make → docker run → make` recursion is bounded by the marker: host targets shell
into the container; container targets do the real work. No infinite loop.

### `make run` flow (target state)

1. `docker build -t nanos-build docker/` — once, then cached. First build ~20-40 min
   (cross-toolchain compilation).
2. `docker run --rm --platform linux/amd64 -v $PWD:/src -w /src nanos-build make _all`
   → produces the `i686-elf` kernel and `fs/image.img` with the kernel written in.
3. Host: `qemu-system-i386 -drive file=fs/image.img,format=raw` — native window.

## Components

| File | Change | Role |
|---|---|---|
| `docker/Dockerfile` | NEW | Debian base; builds `i686-elf` binutils + gcc from source; installs `nasm`, `grub-pc-bin`, `grub-common`, `xorriso`, `mtools` (for `grub-mkrescue`), `e2fsprogs`, `parted`, `make`. Bakes `/etc/nanos-build` marker. Tagged `nanos-build`. |
| `Makefile` | REBUILD | Host/container split via marker; transparent `run`/`run-iso` wrappers; toolchain prefix as a variable; image creation folded in; remove `-fda` and legacy floppy targets; drop hardcoded `/opt/homebrew/...` paths. |
| `scripts/create-image.sh` | SIMPLIFY | Strip the nested `docker build` / `docker run` wrapping (we already run inside Linux). Becomes a pure parted/mke2fs/grub-mkimage sequence invoked from the Makefile container side. |
| `.gitignore` | EXTEND | Add `iso/`, `*.iso`, `fs/image.img` (generated artifacts). |
| `grub.cfg` | KEEP | Multiboot menu entry; minor tuning only if needed. |

### Dockerfile: cross-toolchain build (Approach A)

Standard OSDev recipe inside the image:

- `binutils` — `./configure --target=i686-elf --prefix=/opt/cross --with-sysroot
  --disable-nls --disable-werror && make && make install`
- `gcc` — `./configure --target=i686-elf --prefix=/opt/cross --disable-nls
  --enable-languages=c,c++ --without-headers && make all-gcc all-target-libgcc &&
  make install-gcc install-target-libgcc`
- `/opt/cross/bin` prepended to `PATH`.
- Pin specific binutils/gcc versions for reproducibility.

This yields `i686-elf-gcc`, `i686-elf-as`, `i686-elf-ld` matching the flags already in
the Makefile (`-nostdlib -nostdinc++ --no-exceptions --no-rtti -fno-leading-underscore`,
`nasm -f elf`, linker `-Tlinker.ld -nostartfiles`).

## Data Flow

```
source (.cpp/.s/.S) ──[container: i686-elf toolchain]──► bin/*.o ──[ld -Tlinker.ld]──► bin/kernel.bin
                                                                                            │
grub.cfg + parted/mke2fs/grub-mkimage ──[container]──► fs/image.img ◄──[debugfs write]┘
                                                                  │
                                                       host: qemu-system-i386 (native)
```

ISO path: `bin/kernel.bin` + `grub.cfg` → `iso/` staging → `grub-mkrescue` → `nanos.iso`
→ host `qemu-system-i386 -cdrom nanos.iso`.

## Error Handling

- Container build steps run under `set -e`; any failed stage aborts the build with a
  non-zero exit, surfaced to the host `make`.
- `.gitignore` prevents generated artifacts (`iso/`, `*.iso`, `fs/image.img`) from
  being committed.
- The marker-file detection has a single source of truth; if the marker is absent on the
  host and present in the container, behavior is unambiguous.

## Verification

- **Primary:** `make run` opens a native QEMU window; boot is confirmed visually.
- **Sanity (optional):** headless QEMU with monitor-socket `screendump` (the method used
  during diagnosis) to capture the post-boot screen.
- **Key checkpoint:** if the kernel still does not boot after the Dockerized `i686-elf`
  rebuild, the hypothesis "host toolchain caused it" is falsified, and the next step is
  systematic debugging of the multiboot handoff (kernel load address, multiboot header
  placement in the first 8 KiB, GRUB `multiboot` command output) — tracked separately,
  not assumed away by this change.

## Risks

- First Docker image build is slow (~20-40 min) due to from-source cross-toolchain; this
  is one-time and cached.
- `grub-mkrescue` requires `xorriso` (and often `mtools`) in the image — included.
- Boot failure may have a root cause in the kernel itself, not the toolchain; the design
  explicitly treats that as a follow-up, not a guaranteed fix.
