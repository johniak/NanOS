# 03 - Chromium Content Runtime On NanOS (Coordinator)

**Goal:** Bring up enough Chromium for Electron: base, IPC, content, rendering, input, network, and
local file loading. The first GUI gate is a minimal content shell rendering a local HTML page on
NanOS.

**Why separate from Electron:** Electron failures are easier to debug once Chromium content works
without Electron's Node integration and app layer.

**This file contains no tasks.** Chromium is too large for one execution plan; all technical work
lives in the six child subplans under `03-chromium-content/`, executed strictly in order. Read
[03-chromium-content/README.md](03-chromium-content/README.md) first — it holds the subplan
execution protocol, entry criteria, and per-subplan gates.

**Entry criteria for the whole plan:**
- plan 00 gate green (`chromium_version` pinned in `manifest/electron-stack.lock`);
- plan 01 Tasks 1.2, 1.3, 1.5 green (`epolltest`, `mmapexectest`, `shmdualtest` PASS in QEMU).
- plan 02 may run in parallel, but Chromium and Node work must not edit the same patch files
  concurrently.

| Child plan | Purpose |
|---|---|
| [00-source-and-gn.md](03-chromium-content/00-source-and-gn.md) | Fetch the pinned Chromium, add `target_os = "nanos"` recognition, `gn gen` succeeds. |
| [01-base-ipc-runtime.md](03-chromium-content/01-base-ipc-runtime.md) | Build Chromium `base`, threading, IPC/shared-memory primitives; process-model decision. |
| [02-ozone-nanos.md](03-chromium-content/02-ozone-nanos.md) | NanOS Ozone display/input backend (headless-clone first, then NanWM). |
| [03-content-shell.md](03-chromium-content/03-content-shell.md) | Link and run content shell with local HTML + JS. |
| [04-network-storage-media.md](03-chromium-content/04-network-storage-media.md) | Resource loading, storage paths, fonts/locales, disabled-media policy. |
| [05-debugging-and-regression.md](03-chromium-content/05-debugging-and-regression.md) | Deterministic smoke scripts, log triage, regression gates. |

## Files (owned by the subplans)

**Create:**
- `scripts/electron/fetch-chromium.sh` (subplan 00)
- `scripts/electron/chromium-args-nanos.gn` (subplan 00 — the ONE canonical GN args file)
- `scripts/electron/build-chromium-content.sh` (subplan 03)
- `scripts/smoke-chromium-content.sh` (subplan 05)
- `scripts/chromium-log-triage.sh` (subplan 05)
- `docs/en/chromium-port.md` (subplans 03–05)
- `ports/chromium/README.md`, `ports/chromium/patches/` (subplan 00 onward)
- `Makefile` target `chromium-content` (subplan 03)

**External work dirs:**
- `$(SDK_WORK)/chromium-src/` (checkout; build dir is `out/NanOS` inside it)

## Plan-Level Gate (all subplans complete)

- [ ] `gn gen` and `ninja` are reproducible from a clean checkout.
- [ ] `content_shell.nxe` (or the documented minimal content embedder) loads local HTML on NanOS.
- [ ] JavaScript executes (smoke page DOM mutation observed).
- [ ] Window/input/rendering path is NanOS-owned (Ozone/nanos), documented, and not
  MarkText-specific.
- [ ] `scripts/smoke-chromium-content.sh` is green and proven able to fail.
- [ ] `status.md`: `M3 Chromium Content` checked, log row added.

## Coordinator Checklist

Tick each only when its subplan gate command has passed (record in `status.md`):

- [ ] `00-source-and-gn.md` complete.
- [ ] `01-base-ipc-runtime.md` complete.
- [ ] `02-ozone-nanos.md` complete.
- [ ] `03-content-shell.md` complete.
- [ ] `04-network-storage-media.md` complete.
- [ ] `05-debugging-and-regression.md` complete.
