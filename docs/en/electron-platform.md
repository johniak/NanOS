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

## Platform Gap Matrix

Filled by plan 01 (`01-nanos-platform-gaps.md`). Each row is a NanOS API that Node/Chromium/
Electron needs; status moves `missing → implemented` only with a passing probe/test.

| API | Needed by | Status | Probe / test | Notes |
|---|---|---|---|---|
| _(populated by plan 01)_ | | | | |

Known-missing at freeze time (from the execution guide): `epoll_create1/ctl/wait`, `eventfd`,
`timerfd_*`, `inotify` (header only), real `sigaltstack`, `memfd` seals, `MAP_FIXED_NOREPLACE`.

## How To Port Another Electron App

Filled by plan 05 (`05-electron-app-packager.md`) once the generic packager exists. It will
describe running `scripts/electron/package-app.sh <app-dir>` to produce a
`/disks/main/apps/<app>/` bundle against the shared runtime, with no runtime edits.

## Known Unsupported Electron APIs

Filled by plan 04 (`04-electron-runtime.md`) as APIs are disabled or stubbed. Every entry names the
Electron API, why it is unsupported on NanOS, and the JS-visible behavior (throw / no-op / fallback).

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

## Debugging

Filled by plan 03 subplan 05 (`03-chromium-content/05-debugging-and-regression.md`): how to get
Chromium/Electron logs off NanOS, crash triage, and the regression gate for the content shell.
