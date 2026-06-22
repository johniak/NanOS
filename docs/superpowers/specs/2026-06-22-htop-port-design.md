# htop port for NanOS — design

**Date:** 2026-06-22
**Status:** approved (design), pending implementation plan
**Target arch:** x86_64 (`x86_64-nanos`)
**Backend:** htop's real Linux `/proc` platform

## Goal

Run unmodified upstream **htop 3.x** as a NanOS `.nxe`, using htop's **Linux `/proc`
backend**, so the meter header (CPU / memory / load) and the live process list show
real data. Same reproducible port flow as `make ping` / `make wget` / `make vim`
(`nanos-port` driver + `nxport.toml`, built inside the `nanos-sdk-dev` container).

Non-goals: htop's optional columns that depend on `/proc` files NanOS does not expose
(`io`, `smaps_rollup`, `cgroup`, `oom_score`, per-task thread view). htop guards these
and disables the corresponding columns when the files are absent — acceptable.

## Why this is straightforward

- **ncurses is already ported.** `libncurses.a` + `libtinfo.a` + `curses.h`/`term.h`
  live in the SDK sysroot (`~/Projects/nanos-sdk/toolchain/x86_64-nanos/lib`, built by
  `make ARCH=x86_64 ncurses`); the vim port already links them.
- **NanOS already has a Linux-style `/proc`** (`fs/SynthFs.cpp`): `meminfo`, `stat`
  (with `cpu`/`cpu0` lines, `ctxt`, `btime`, `processes`, `procs_running/blocked`),
  `loadavg`, `cpuinfo`, `uptime`, `version`, and per-pid `comm`/`cmdline`/`stat`/
  `status`. A comment in `SynthFs.cpp` already notes those fields are "the ones
  top/htop/ps read".

So htop is the sibling of the vim port: an ncurses-linking autotools app, cross-built
to `x86_64-nanos`, reading `/proc`.

## 1. Build mechanism (mirrors `make vim`)

- New port dir `~/Projects/nanos-sdk-work/htop-port/`:
  - `src/` — the htop 3.x release tarball unpacked (the `source = "dir:src"`
    convention used by `ncurses-port`).
  - `nxport.toml` — `build = "autotools"`, ncurses configure flags, cross-compile
    `cache` for htop's `AC_TRY_RUN` probes, `needs = ["libc.ndl"]`, `install =
    "/nanos/bin"`.
  - `hooks/` — `pre_configure.sh` (see §2).
- New `make htop` target (x86_64 branch), structured exactly like the `make vim`
  x86_64 branch:
  1. `$(NXPORT_PREREQ)` — rebuild in-tree `libc.ndl{,.a}`, `crt0.o`, `nxhdr.o`,
     `mknx64`.
  2. Refresh the `x86_64-nanos` sysroot from this checkout (libc-glue headers,
     `SyscallNr.h`, `nx-dllimport.h`, `libc.a`/`libc.ndl`, `crt0.o`/`nxhdr.o`,
     `x86_64-nanos-mknx`), and assert `libtinfo.a` is present (run
     `make ARCH=x86_64 ncurses` first).
  3. Run `nanos-port` in the `nanos-sdk-dev` container against `htop-port`.
  4. `cp htop.nxe bin/htop.nxe`.
- `make image64`'s `_image64` installs `bin/htop.nxe` into `/nanos/bin/htop`. As with
  every port, `make image*` never *depends* on `make htop` — a missing artifact is
  silently skipped.

## 2. Teaching htop's build to use the Linux backend

htop's `configure` selects a platform from `$host_os`; `x86_64-nanos` is unknown and
falls back to the do-nothing "unsupported" platform. Honest fix (NanOS genuinely
exposes a Linux `/proc`): `hooks/pre_configure.sh` patches `configure.ac`'s platform
`case` to map `*nanos*` → the `linux` platform, then runs `autoreconf -fi` (autotools
are available in the container). This makes htop compile its `linux/*.c` backend
(`LinuxProcessTable`, `LinuxMachine`, the `/proc` readers) via the `HTOP_LINUX`
automake conditional. The release tarball ships a generated `configure`, but
regenerating after the `configure.ac` edit is the clean lever (vs. hand-patching
generated `configure`/`Makefile.in`).

## 3. NanOS `/proc` gaps to fill (in a git worktree off `develop`)

Per the "full Linux backend" decision, extend `fs/SynthFs.cpp` and
`kernel/Process.{h,cpp}` (the `ProcInfo` struct + its populator). All work happens in
a **git worktree** off `develop` (the htop port artifacts live in `nanos-sdk-work`,
outside the repo; only these kernel/fs changes touch the NanOS tree).

| htop reads | NanOS today | Change |
|---|---|---|
| `/proc/<pid>/statm` (size, resident, share, text, lib, data, dt) | **absent** (not in `PROC_FILES`) | add a renderer + add `statm` to `PROC_FILES` |
| `/proc/<pid>/stat` fields 23–24 (vsize, rss) | hardcoded `0 0` | fill from the process's real address-space size |
| `/proc/<pid>/status`: `Uid`, `Gid`, `VmSize`, `VmRSS`, `Threads` | only Name/State/Pid/PPid/Pgid/Sid/Kthread | add those lines (drives htop's USER + per-proc mem columns) |
| `/proc/meminfo`: `MemAvailable`, `Buffers`, `Cached`, `SwapTotal`, `SwapFree` | `MemTotal`/`MemFree`/`MemUsed`/`KHeapTotal`/`KHeapFree` | add the keys htop's mem meter expects (`Buffers`/`Cached`/swap = 0; `MemAvailable` ≈ `MemFree`) |
| `ProcInfo` lacks RSS / VSZ / nthreads / uid | fields absent | add fields, populate from the process / address space |

Verify (no change expected, just confirm): `/proc/stat` `cpu`/`cpu0` lines give htop
its CPU count and times; `/proc/loadavg` and `/proc/uptime` feed the load + uptime
meters; `openat`/`dirfd` on the `/proc` directory work from a real port (htop iterates
`/proc` with `dirfd` + `openat`, not plain `open`).

The `/proc` renderers are pure functions with existing doctest coverage in the
`SynthFs` host tests; extend those tests for the new `statm`/`status`/`meminfo`
fields (TDD: failing test first).

## 4. Install location

`/nanos/bin/htop` — a system monitor in the `top`/`free` family, so flat in the
system `bin` (not an `/apps/<name>` bundle like vim/doom).

## 5. Verification

QEMU x86_64: `make image64` then boot (`make run64` or the headless
screendump-over-monitor pattern from CLAUDE.md). Boot to the shell, run `htop`, and
confirm: the meter header renders, the process list shows live processes with
non-zero CPU/memory, navigation works, and `q` quits cleanly. Re-run `e2fsck` is not
needed (htop is read-only w.r.t. the disk).

## Risks / open questions

- **`autoreconf` version skew** — htop's `configure.ac` may require a newer autoconf
  than the container ships; if so, pin the platform patch to the generated `configure`
  + `Makefile.in` instead of regenerating.
- **`openat`/`dirfd` on `/proc`** — must be confirmed working from a real port; the
  inetutils port deliberately *stubbed out* gnulib's `dirfd`/`fdopendir`, so this path
  has not necessarily been exercised against the synthetic `/proc` dir.
- **htop platform probes** — some `AC_TRY_RUN` checks in htop's Linux platform may need
  explicit `cache` entries; collected during the first build.
