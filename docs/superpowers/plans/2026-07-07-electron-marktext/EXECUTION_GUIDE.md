# Execution Guide For Junior Implementers

This file explains how to execute the Electron/MarkText plan without guessing the process. The
individual child plans define the technical work; this guide defines workflow, handoff, naming
contracts, verified platform facts, and what to do when a step fails. Read it fully before touching
any child plan. Child plans refer back here instead of repeating this material.

## Rule Zero

Do not start with MarkText. MarkText is the final acceptance app. The platform is ready for MarkText
only after these gates pass in order:

1. source lock exists;
2. platform gap matrix exists;
3. `node.nxe` runs JavaScript;
4. Chromium content smoke renders local HTML;
5. `electron-hello` opens a window;
6. a second generic Electron app packages without MarkText code;
7. MarkText packages and runs.

If any earlier gate is red, work there first.

## NanOS Facts You Must Know (verified against the tree 2026-07-07)

Do not rediscover or re-implement these. If reality disagrees with a row below, fix this file in
the same commit that lands your change.

### Build system

- Everything kernel/userland builds inside Docker, but the host entry points hide that:
  - `make image64` — builds the x86_64 disk image `disk/image64.img`;
  - `make test64` — host unit tests (~863, must stay green);
  - `make verify64` — test64 plus every QEMU smoke gate;
  - `make run64` — boots the image in a QEMU window.
  These targets set `ARCH=x86_64` internally. Run them from the repo root with no extra flags.
- Ported third-party software builds through the SDK container. Pattern to copy: the `grep`,
  `toybox`, and `vim` blocks near the top of `Makefile` —
  `$(NXPORT_RUN) -v "$(SDK_WORK)/<src>":/work/src $(DOCKER_IMAGE) sh /src/scripts/nx-port-build.sh
  <recipe>`, then `cp` the produced `.nxe` into `bin/`.
- `SDK_WORK` defaults to `$(HOME)/Projects/nanos-sdk-work` (`Makefile` ~line 91). Big external
  checkouts (Chromium, Electron, Node, MarkText) live there. **Never vendor them into this repo.**
- **Build hardware reality check (Chromium/Electron only).** A Chromium build is 30–50k compile
  jobs. Running it inside an x86_64 Docker container emulated on an Apple Silicon Mac is NOT
  viable (days-to-weeks per build). Before starting plan 03, secure a **native x86_64 Linux
  builder** (workstation or cloud box: 16+ cores, 64 GiB RAM, 250 GiB free SSD) and run the
  Chromium/Electron `fetch`/`gn`/`ninja` steps there; only the final binaries come back for
  `.nxe` conversion and image staging. `ccache` is mandatory (wired in the canonical GN args).
  Node (plan 02) is ~100× smaller and can stay in the normal SDK flow.
- **Fast QEMU iteration loop.** Rebuilding one `.nxe` does not require a full `make image64`:
  inject the changed binary into the existing image (loopback/debugfs write, same trick
  `scripts/arm-i915.sh` and `scripts/push-dell.sh` use on the stick, or `scripts/qemu-drive.py`)
  and reboot QEMU. During Chromium bring-up (a binary rebuilt hundreds of times) add a small
  `scripts/electron/push-qemu.sh <file> <image-path>` helper the first week — it pays for itself
  immediately.
- To add a user program or microtest: create `user/<name>.c`, then search the Makefile for
  `mmaptest` and mirror its three registration sites — the `USER_PROGS` list, the
  `$(BINFOLDER)<name>.nxe: $(DYN_DEPS) $(BINFOLDER)<name>.o` link rule, and the x86_64 subset
  (`X64_SYS_PROGS`, which feeds `X64_USER_PROGS`). Rebuild with `make image64`; the binary appears
  as `/disks/main/bin/<name>.nxe` inside NanOS.

### Runtime surface already present — do NOT re-implement

- **pthreads:** full musl-derived pthread port on picolibc (create/join, mutexes, condvars, TLS),
  plus a real `munmap`.
- **Processes:** `fork`, `execve`, `wait*`, process groups/sessions, full signals + job control.
- **Dynamic modules:** Windows-style `.ndl` modules with import-by-name; `libc.ndl` is a complete
  C library. There is **no ELF `.so`/`dlopen` loader**: `dlopen()` in
  `user/libc-glue/posixstubs.c` always returns NULL by design. Plan around this (static native
  modules), do not "quickly add dlopen".
- **`memfd_create` exists** as a file-backed emulation (unlinked unique file under `/tmp`, which
  is RamFs — so it is memory-backed in practice). No `F_ADD_SEALS`. See
  `user/libc-glue/posixstubs.c`.
- **`sigaltstack` is a stub** that reports "disabled"; signal handlers run on the normal stack.
- **Filesystem:** ext2/ext4 read+write (JBD2 journaling), `/proc` subset good enough for htop
  (`/proc/<pid>/stat` with 52 fields, `/proc/<pid>/task`, `/proc/cpuinfo`, `/proc/meminfo`),
  `/dev/urandom` + `getrandom` (real CSPRNG), PTYs, `/dev/fb0`, DRM device nodes.
- **Network:** full TCP/IP stack, DNS resolver, OpenSSL 3.0.15 with verified TLS client and
  server (wget-https, Dropbear SSH server work).
- **Graphics:** NanWM window manager with `libnw`/`libnwui` client libraries, software fbdev path,
  GL path via virgl on QEMU. TTF fonts via stb_truetype (IBM Plex Sans, JetBrains Mono bundled).
- **SMP:** 4-core QEMU with fine-grained locking (no big kernel lock).

### Known missing — expected to be implemented in plan 01

- `epoll_create1`/`epoll_ctl`/`epoll_wait` — absent (no hits in `kernel/` or `user/libc-glue/`).
- `eventfd` — absent.
- `timerfd_*` — absent (implement only when a probe demands it; see plan 01).
- `inotify` — header only (`user/libc-glue/include/sys/inotify.h`), no implementation.
- Real `sigaltstack`, memfd seals, `MAP_FIXED_NOREPLACE` — absent.

## Vocabulary And Path Contract

- **Repo** = this NanOS git repository. **Image** = `disk/image64.img` from `make image64`.
- Paths like `/disks/main/...` are paths **inside a running NanOS** (the mounted main disk).
  `/nanos/lib/electron/` and `/disks/main/nanos/lib/electron/` name the same directory; plans use
  the `/disks/main/...` spelling. At image-build time those files are staged from the repo
  (`bin/`, `disk-content/`, `config/`) by the `_image64` recipe — when a plan says "install X to
  /disks/main/apps/foo/", add a staging rule there, following how `disk-content/www` and
  `apps/doom` are installed today.
- `$SDK_WORK` (shell) and `$(SDK_WORK)` (Makefile) are the same external work dir.
- File associations live at `/disks/main/nanos/config/associations.conf`, edited in
  Settings → Default Apps (see `docs/en/windowing.md`). There is **no** `config/etc/associations.conf`.
- Example Electron apps live in the repo under `examples/` (`examples/electron-hello/`,
  `examples/electron-notes-smoke/`); smoke pages under `disk-content/`.

## First Session Checklist

Start here on a fresh branch:

```sh
git status --short
git switch -c feat/electron-platform-marktext   # or switch to it if it already exists
```

Then read, in this exact order:

```text
docs/superpowers/plans/2026-07-07-electron-marktext/README.md
docs/superpowers/plans/2026-07-07-electron-marktext/00-platform-contract.md
docs/en/writing-apps.md
docs/en/nxe-ndl.md
docs/en/windowing.md
docs/en/graphics.md
docs/en/linuxkpi.md
```

Do not edit code until Task 0 from `00-platform-contract.md` is complete.

## Work Package Protocol

Every work package must end with these artifacts:

- source files or docs changed;
- one gate command and its result;
- one status entry in `status.md`;
- notes for limitations or skipped tests.

Use this status format:

```md
| 2026-MM-DD | <commit-or-wip> | <gate name> | PASS/FAIL + one-line detail |
```

When a gate fails, do not move to the next child plan. Add a `FAIL` status row, fix the failing
layer, then rerun the same gate.

## Decisions Are Closed

Each child plan contains a **Decisions** section. Those decisions are made; do not re-open them or
pick a different option because it looks easier. If a decision proves technically impossible,
treat it as a blocker (see Blocker Rules), record the evidence, and update the plan file in the
same commit that changes course — never diverge silently.

## Dependency Handoff

Each child plan produces a named handoff artifact consumed by the next one.

| Producer | Handoff Artifact | Consumer |
|---|---|---|
| `00-platform-contract.md` | `manifest/electron-stack.lock`, bundle layout docs | all later plans |
| `01-nanos-platform-gaps.md` | platform gap matrix + implemented syscall/device tests | Node and Chromium |
| `02-node-v8.md` | `node.nxe`, Node config notes, native addon policy | Electron and MarkText |
| `03-chromium-content.md` (coordinator for `03-chromium-content/*`) | content shell smoke + Ozone/NanOS backend | Electron runtime |
| `04-electron-runtime.md` | `electron.nxe`, runtime resources, hello app smoke | generic packager |
| `05-electron-app-packager.md` | `package-app.sh`, second app smoke | MarkText |
| `06-marktext-port.md` | MarkText bundle + smoke result | verification plan |
| `07-verification-and-ci.md` | `verify-electron` gate | final acceptance |

If a handoff artifact does not exist, the next plan is blocked. Plan 03 is itself split into six
subplans under `03-chromium-content/`; their internal handoffs are listed in
`03-chromium-content/README.md`.

## Command Name Contract

These names are the contract. All are run from the repo root on the host. Existing targets are
marked; the rest are created by the plan that owns them.

```sh
make test64                          # exists — host unit tests
make image64                         # exists — build x86_64 image
make verify64                        # exists — full existing gate suite
make node                            # plan 02 — build node.nxe via SDK container
scripts/smoke-node.sh                # plan 02
make chromium-content                # plan 03 — build content shell + runtime files
scripts/smoke-chromium-content.sh    # plan 03 (subplan 05)
make electron-runtime                # plan 04 — build electron.nxe + runtime resources
make electron-hello                  # plan 04 — package the hello app
scripts/smoke-electron-hello.sh      # plan 04
make electron-notes-smoke            # plan 05 — package the second proof app
scripts/smoke-electron-packager.sh   # plan 05
make marktext                        # plan 06 — build + package MarkText
scripts/smoke-marktext.sh            # plan 06
make image64-electron                # plan 07 — image with runtime + hello + MarkText
make verify-electron                 # plan 07 — runs every smoke above
```

If a command name must change, update this guide, the relevant child plan, and
`07-verification-and-ci.md` in the same commit.

## QEMU Smoke House Style

Every smoke script in this program copies the conventions of `scripts/smoke-bigmem.sh` (read it
before writing your first smoke):

- `#!/usr/bin/env bash` + `set -u`; a comment header saying what the smoke proves;
- boot `disk/image64.img` headless: `-display none -serial file:"$SER"`, QEMU monitor on a unix
  socket, `-no-reboot`, `-d int,cpu_reset -D "$INT"` for the fault log;
- **bounded waits only** — `for i in $(seq 1 N); do grep -q ... && break; sleep 1; done`; never an
  unbounded `sleep`/`wait`;
- type into the guest via monitor `sendkey` (see the embedded python in `smoke-bigmem.sh`; space
  is `spc`, `/` is `slash`); log in as `jan`/`jan`;
- assert with `chk`/`no` helpers grepping the serial log; always include a fault check
  (`no "Page fault|General protection|PANIC" ...`);
- kill only your own QEMU PID (`trap 'kill -9 "$QPID"' EXIT`), never a broad `pkill` without the
  image-path filter;
- exit 0 on pass, 1 on fail, 2 on missing prerequisites (e.g. image not built).

Every smoke must also prove it can fail: a deliberately broken input (wrong marker, missing file)
must make it exit non-zero. Do this once when writing the script and note it in the commit message.

## How To Handle Missing NanOS APIs

When Node/Chromium/Electron needs a missing syscall or device feature:

1. Add it to the gap matrix in `docs/en/electron-platform.md`.
2. Write a small NanOS user test first, for example `user/epolltest.c` (registration recipe in
   "NanOS Facts" above).
3. Add host tests for pure bookkeeping where possible (`tests/`).
4. Implement the kernel/libc-glue change.
5. Run `make test64`.
6. Run the user smoke in QEMU.
7. Update the gap matrix row from `missing` to `implemented`.

Do not add broad Linux compatibility shims without a failing probe or test that needs them.

## How To Handle Upstream Patches

**Relation to the repo-reorg plan (recipe publishing).** The port-recipe publishing mechanism is
defined by `docs/superpowers/plans/2026-07-02-plan-1-repo-reorg-manifest-bootstrap.md` (K1, not
yet executed): canonical recipes live in the sibling `nanos-sdk` repo under `ports/<name>/`
(`nxport.toml`, hooks, patches), and `nanos-fetch` materializes `$SDK_WORK/<name>-port/` work
dirs from pinned URL+SHA256. **Recommended order: land Plan 1 first** (it is the top tech-debt
priority anyway). If Plan 1 is done, every `ports/<name>/` path below means
`nanos-sdk/ports/<name>/` and fetch scripts become `nanos-fetch` recipes; if the Electron program
starts earlier, keep the paths repo-local as written and migrate them during Plan 1's rescue
sweep — either way the recipes are versioned, never only in `nanos-sdk-work`.

Patch location rules:

- Node patches: `ports/node/patches/`
- Chromium patches: `ports/chromium/patches/`
- Electron patches: `ports/electron/patches/`
- Generic packager patches/scripts: `scripts/electron/`
- MarkText-only patches: `ports/marktext/patches/`

Every patch file needs a short header:

```text
Reason: <what NanOS lacks or what upstream assumes>
Scope: platform | node | chromium | electron | marktext
Upstreamability: yes/no + why
Gate: <smoke/test that fails without this patch>
```

If the same fix is useful to two apps, it is not MarkText-only.

## Blocker Rules

A worker may mark a task blocked only when all are true:

- the exact failing command is recorded;
- the last 50 useful log lines are saved in the status notes or linked artifact;
- the suspected layer is named;
- a smaller reproducer has been attempted.

Examples:

- Good blocker: `node.nxe -e` faults at `mprotect(PROT_EXEC)`; `mmapexectest` reproduces.
- Bad blocker: `Electron does not work`.

## Review Checklist Before MarkText Work Starts

Before opening `06-marktext-port.md`, confirm:

- [ ] `manifest/electron-stack.lock` pins exact Electron/MarkText versions.
- [ ] `node.nxe -e "console.log(1+2)"` works on NanOS.
- [ ] Chromium content smoke runs JavaScript and renders a window.
- [ ] `electron-hello` passes IPC/file-write smoke.
- [ ] `electron-notes-smoke` packages through the generic packager.
- [ ] Runtime patches contain no MarkText references.

If any box is unchecked, MarkText work is premature.

## Final Review Checklist

The final PR is acceptable only if:

- [ ] `scripts/smoke-marktext.sh` edits and saves a markdown file.
- [ ] a second Electron app still packages and runs.
- [ ] docs explain how to package another Electron app.
- [ ] no source version is floating.
- [ ] every disabled Electron/MarkText feature is documented.
- [ ] no new kernel API is untested.
- [ ] all status rows are up to date.
