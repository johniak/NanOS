# Electron Apps on NanOS — Platform Reference

This is the contract for running Electron applications on NanOS as first-class `.nxe` app
bundles. The acceptance app is **MarkText**, but the platform is designed so any normal Electron
app packages against a **shared** Electron runtime without runtime edits.

Program plan: `docs/superpowers/plans/2026-07-07-electron-marktext/`. Execution rules and verified
NanOS facts: that directory's `EXECUTION_GUIDE.md`. Status: that directory's `status.md`.

## Source/Version Policy

Every version this program builds is frozen in [`manifest/electron-stack.lock`](../../manifest/electron-stack.lock).
Nothing floats at build time — no `latest`, no `^`/`~`. Changing a pin means redoing plan 00 and
updating the lock in the same commit; all downstream plans key off it.

Current freeze (2026-07-13):

| Component | Pin |
|---|---|
| MarkText | `43bd8b77795fb27b1a9512737c000f7362031ea0` (v0.20.0-dev) |
| Electron | `42.1.0` (tag `v42.1.0`) |
| Chromium | `148.0.7778.97` |
| Node | `v24.15.0` |

**Fork policy.** Per the NanOS-labs ecosystem decision (2026-07-12), any upstream we patch becomes
a fork under `github.com/NanOS-labs` on branch `nanos`, with the port recipe (build scripts, hooks,
patches) inside the fork. This **supersedes** the plan/guide wording that put patches in repo-local
`ports/<name>/patches/`. Concretely:

| Plan wording | Actual location |
|---|---|
| `ports/node/patches/` | `NanOS-labs/node-nanos` (branch `nanos`) |
| `ports/chromium/patches/` | `NanOS-labs/chromium-nanos` |
| `ports/electron/patches/` | `NanOS-labs/electron-nanos` |
| `ports/marktext/patches/` | `NanOS-labs/marktext-nanos` |
| `scripts/electron/` (generic packager) | stays in the NanOS repo — it is platform glue, not an upstream fork |

None of these forks exist yet at M0 (source freeze only); each is created when its plan (02+)
first needs to patch. Big source checkouts still live in `$SDK_WORK` and are never vendored into
the NanOS repo.

**Sanctioned contingency (Version Downshift).** Chromium `148` is a large, recent, clang/sandbox-
entangled milestone. If plan 03 subplan 01 cannot get Chromium `base` past its blocker after the
error census, the approved fallback is pinning an older MarkText release with an older Electron
major (smaller Chromium). That is a project-owner decision — raise it as a blocker with the census
attached, then redo plan 00 with new pins. See the program README "Sanctioned Contingency".

## Runtime And Bundle Layout

One Electron runtime is shared by every app. Paths are inside a running NanOS (the mounted main
disk); `/nanos/lib/...` and `/disks/main/nanos/lib/...` name the same directory.

Runtime install layout:

```text
/disks/main/nanos/lib/electron/
  electron.nxe
  resources/
  icudtl.dat
  locales/
  snapshot_blob.bin
  v8_context_snapshot.bin
```

App layout (one directory per app, sharing the runtime above):

```text
/disks/main/apps/<app>/
  <app>.nxe                 # tiny launcher that execs the shared runtime (plan 05)
  app.asar                  # preferred app payload
  package.json
  resources/
  native/                   # NanOS-built native modules, if any
  nanos-app.toml            # NanOS-specific metadata
```

`nanos-app.toml` schema (example values for MarkText):

```toml
name          = "marktext"
display_name  = "MarkText"
runtime       = "electron"
main          = "app.asar"
electron_min  = "42.1.0"
argv          = []
env           = { ELECTRON_DISABLE_SECURITY_WARNINGS = "1" }
file_associations = ["md", "markdown", "txt"]
```

Launch contract:

- `/disks/main/bin/<app>.nxe` (link-farm entry) execs
  `/disks/main/nanos/lib/electron/electron.nxe` with the app directory as its argument.
- `open file.md` routes to the app through the existing associations mechanism —
  `/disks/main/nanos/config/associations.conf`, edited in **Settings → Default Apps**
  (see [windowing.md](windowing.md)). The packager appends rows there at install time from the
  app's `file_associations`.
- From NanWM, app launch uses the existing `open`/launch socket path.

## App-Port Boundary

The rule that keeps this a platform and not a MarkText special case:

- **Platform/runtime code** may live only in the Electron/Chromium/Node/NanOS platform layers:
  the NanOS-labs forks `node-nanos`, `chromium-nanos`, `electron-nanos`, the generic packager
  under `scripts/electron/`, and NanOS kernel/libc-glue. If a fix helps two apps, it belongs here,
  never in an app fork.
- **App-specific patches** may live only in `NanOS-labs/marktext-nanos` (branch `nanos`), with a
  `NANOS_NOTES.md` recording every app-local workaround and why it is not platform code.
- The generic packager must package a **second** tiny Electron app — `electron-notes-smoke`
  (built in plan 05) — with no MarkText code, before MarkText is considered platform-clean.

## Native Module Policy

NanOS has **no ELF `.so`/`dlopen` loader** (`dlopen()` returns NULL by design; dynamic linking is
the Windows-style `.ndl` mechanism). Therefore:

- Required native npm modules must be rebuilt for `x86_64-nanos` and **statically registered** into
  the Electron runtime (mechanism defined by plan 02 Task 2.5). They cannot be loaded at runtime as
  `.node`/`.so` files.
- Optional native modules may be disabled only if the app has an equivalent JS fallback.
- Every disabled optional module gets an entry in `NanOS-labs/marktext-nanos` `NANOS_NOTES.md`
  (app-local) or the runtime notes (if platform-wide).

## Runtime Data Files (fonts, ICU, locales, certificates)

Chromium/Electron load several data files at startup. Where each lives on NanOS (paths inside a
running NanOS), reusing existing bundles where they already exist:

| Data | Path | Status |
|---|---|---|
| ICU data | `/disks/main/nanos/lib/electron/icudtl.dat` | staged with the runtime (plan 04) |
| Locale packs | `/disks/main/nanos/lib/electron/locales/` | staged with the runtime (plan 04) |
| V8 snapshots | `/disks/main/nanos/lib/electron/{snapshot_blob,v8_context_snapshot}.bin` | staged with the runtime (plan 04) |
| Fonts | `/nanos/share/fonts/` (already in every image: IBM Plex Sans, JetBrains Mono; staged from `assets/fonts/` by the `_image64` recipe, `Makefile` ~1583) | **exists today** |
| CA certificates | `/nanos/ssl/cert.pem` (the OpenSSL/TLS bundle, staged from `disk-content/ssl/cert.pem`, `Makefile` ~1695) | **exists today** |

**Font discovery is deterministic.** Chromium gets one fixed font directory (`/nanos/share/fonts/`)
wired through Skia in plan 03 subplan 04 — no host paths, no fontconfig scan. Bundling more faces
(a wider Unicode range than the two UI fonts) is a plan-03/04 decision; the directory is the
contract. The runtime data files under `/disks/main/nanos/lib/electron/` do not exist until plan 04
builds the runtime; fonts and certs are present in a freshly built image now.

## Platform Gap Matrix

Owned by plan 01 (`01-nanos-platform-gaps.md`). Each row is a NanOS API that Node/Chromium/Electron
needs; a `missing-required` row moves to `implemented` only with a passing QEMU microtest. Statuses:
`implemented`, `implemented-but-insufficient`, `missing-required`, `disabled-by-flag`,
`disabled-by-policy`, `not-needed-yet`.

Evidence re-verified against the tree on **2026-07-13** (grep commands below each cite the file:line
or the emptiness that proves the row).

### Event loop / IPC readiness

| Feature | Status | Evidence | Consumer |
|---|---|---|---|
| `eventfd` | implemented | `kernel/Eventfd.h` + dispatch `SYS_eventfd2`; `scripts/smoke-eventfd.sh` PASS | libuv/Chromium message pump |
| `epoll_create1/ctl/wait` | implemented (level-triggered) | `kernel/Epoll.h` + dispatch; `smoke-eventfd.sh` epolltest PASS | libuv/Chromium message pump; EPOLLET accepted-not-honored |
| `timerfd_*` | not-needed-yet | grep empty; Decisions: timers via `epoll_wait` timeout | libuv timers |
| `pipe2` | missing-required | not in `kernel/SyscallNr.h` (only `SYS_pipe`) | libuv pipes with `O_CLOEXEC\|O_NONBLOCK` |
| `dup3` | missing-required | not in `kernel/SyscallNr.h` (only `SYS_dup`/`SYS_dup2`) | libuv fd setup |
| `accept4` | implemented | `SyscallNr.h:44 SYS_accept4=288`; `sockets.c:55` | Chromium net service |

### Memory / V8

| Feature | Status | Evidence | Consumer |
|---|---|---|---|
| `mprotect` RW↔RX flip | implemented | `arch::mmuProtectUser` + `AddressSpace::protect` (toggles PTE_RW + TLB shootdown); `SYS_mprotect`; `scripts/smoke-mprotect.sh` (mmapexectest) PASS | V8 W^X code gen. NX not enabled, so PROT_EXEC is a no-op and write-protection is via PTE_RW; absent pages skipped (no ENOMEM) to not regress guard-page callers |
| `madvise` | implemented (advisory no-op) | `include/sys/mman.h:42` | V8 GC hints |
| `mincore` | implemented (all pages resident; no reclaim) | `posixstubs.c:466` | Chromium |
| `memfd_create` | implemented (uniquely-named `/tmp` file, kept linked, no seals) | `posixstubs.c`; `scripts/smoke-memfd.sh` PASS | Chromium shared memory. NOT unlinked-anonymous (NanOS tracks fds by path, so an unlinked-open file loses ftruncate/mmap); the `/tmp` entry stays until close |
| `MAP_SHARED` cross-process writeback | **missing-required** | shmdualtest hangs: a file mmap allocates fresh private pages eagerly filled from the file (`SyscallDispatch.cpp` ~1023) — MAP_PRIVATE semantics, no write-back, no page sharing | Chromium shmem — remaining Task 1.5 work: page-frame-backed RamFs + shared mapping |
| `MAP_FIXED_NOREPLACE` | not-needed-yet | absent; add only if V8 build probes (matrix row first) | V8 heap placement |
| `F_ADD_SEALS` (memfd seals) | not-needed-yet | absent; add only if Chromium rejects unsealed memfds | Chromium shmem |

### Process / path introspection

| Feature | Status | Evidence | Consumer |
|---|---|---|---|
| `fork`/`execve`/`wait*` | implemented | multiprocessing stack | Electron child processes |
| pthreads / TLS / futex | implemented (musl port, real munmap) | pthread port | V8/libuv threading |
| `/proc/self/exe` readlink | implemented | `SynthFs::readlink` + `Process.exe` (set at execve); `scripts/smoke-procself.sh` PASS | Electron app-path resolution |
| `/proc/self/fd` listing | implemented | `SynthFs::readdir` fd dir + `ProcTable::openFds`; smoke-procself PASS | Chromium fd introspection |
| `/proc/self` resolution | implemented | `classifyProc` maps `self`→`ProcTable::selfPid()` | all of the above |
| user-writable `/tmp` | implemented | `/tmp` now mounts `RamFs(01777)` (sticky, world-writable); `scripts/smoke-memfd.sh` PASS | Node/Electron temp files + memfd |
| `/proc/<pid>/stat` (52 fields), `/task`, `cpuinfo`, `meminfo` | implemented (htop-level) | htop port; `SynthFs.cpp` | Node `os`, Chromium |
| `statfs`/`fstatfs` | implemented | `syscalls.c:226` | Node `fs` |
| `fstatat` + `*at` family | implemented | `syscalls.c:867` | Node `fs` |
| `statx` | not-needed-yet | absent; Node/glibc fall back to `fstatat` | Node `fs` |
| `clock_gettime` MONOTONIC/REALTIME | implemented | networking-stack fix | libuv timers |
| `getrandom`, `/dev/urandom` | implemented (real CSPRNG) | TLS/SSL stack | Chromium RNG |

### Deliberately not provided (policy / by-design)

| Feature | Status | Evidence | Consequence |
|---|---|---|---|
| `dlopen` | missing-required-by-design (returns NULL; `.ndl` model) | `posixstubs.c` + `include/dlfcn.h` | native modules must be statically registered (plan 02 Task 2.5) |
| `inotify` | disabled-by-policy (polling watchers) | header only (`include/sys/inotify.h`) | launcher sets `CHOKIDAR_USEPOLLING=1` (plan 05) |
| `sigaltstack` | disabled-by-flag (stub reports disabled) | `posixstubs.c`; Decisions: V8 stack checks are limit-based | build V8 with wasm trap-handler off (plans 02/03) |
| Chromium Linux sandbox syscalls (seccomp/namespaces) | disabled-by-flag | none present | `--no-sandbox` runtime default (plan 05 launcher); security limitation (Task 1.8) |

**Work list for Tasks 1.2–1.5** (the `missing-required` rows): `eventfd`, `epoll`, `pipe2`, `dup3`,
`mprotect` real RW↔RX, `/proc/self/exe`, `/proc/self/fd`, and proving `MAP_SHARED` writeback.

## How To Port Another Electron App

Filled by plan 05 (`05-electron-app-packager.md`) once the generic packager exists. It will
describe running `scripts/electron/package-app.sh <app-dir>` to produce a
`/disks/main/apps/<app>/` bundle against the shared runtime, with no runtime edits.

## Known Unsupported Electron APIs

Plan 04 (`04-electron-runtime.md`) fills this per-API as things are disabled or stubbed. Each entry
names the Electron API, why it is unsupported on NanOS, and the JS-visible behavior (throw / no-op /
fallback).

### Chromium sandbox — disabled (security limitation)

NanOS does not provide Chromium's Linux sandbox primitives (seccomp-bpf, user namespaces, the
setuid/namespace helper). The sandbox is therefore **off**:

- **Runtime:** the NanOS Electron launcher passes `--no-sandbox` by default (plan 05 launcher). It
  is a runtime/launch default, **never** set per-app.
- **Build:** the GN args disable the sandbox at build time on the unsupported-platform path
  (`is_official_build=false`; sandbox off) — plan 03 subplan 00 owns the exact GN args.
- **Consequence:** renderer processes run without OS-level isolation. Malicious or exploited web
  content is not contained the way it is on Linux. Acceptable for the MarkText acceptance app
  (trusted local content); documented here as a real limitation for any app loading remote content.
- **Follow-up:** a real NanOS sandbox (a capability/namespace analogue Chromium can target) is a
  future platform project, out of scope for this program.

## Acceptance

Final gate script (contract defined now, implemented in plan 06): `scripts/smoke-marktext.sh`.

1. Build/install the MarkText bundle into `disk/image64.img`.
2. Boot NanOS with GUI enabled.
3. Launch MarkText from the desktop or command line.
4. Open `/disks/main/apps/marktext-smoke/input.md`.
5. Insert a line `NanOS Electron smoke OK`.
6. Save.
7. Quit MarkText.
8. Reopen the file with a simple CLI tool and assert the saved line is present.
9. Fail if QEMU logs any page fault, general protection fault, assert, or Electron fatal error.

The smoke follows the house style in the execution guide "QEMU Smoke House Style" (headless boot,
bounded waits, fault check, provably-can-fail).

## File Watching Policy

NanOS has no `inotify` (header only; see the gap matrix), by decision — file watching is
**polling-based** at the platform level, not a per-app patch:

- The NanOS Electron launcher sets `CHOKIDAR_USEPOLLING=1` (and, where relevant,
  `UV_USE_IO_URING=0`) in the runtime environment (plan 05 launcher), so libuv/chokidar fall back
  to stat-polling watchers instead of `inotify`. Plan 06 must **not** patch this into MarkText.
- **Idle-CPU contract:** polling watchers must not spin the CPU. The final `scripts/smoke-marktext.sh`
  (plan 06) asserts that an idle MarkText under polling stays below an agreed CPU threshold — a
  regression guard so the polling fallback never turns into a busy loop.

## Debugging

Filled by plan 03 subplan 05 (`03-chromium-content/05-debugging-and-regression.md`): how to get
Chromium/Electron logs off NanOS, crash triage, and the regression gate for the content shell.
