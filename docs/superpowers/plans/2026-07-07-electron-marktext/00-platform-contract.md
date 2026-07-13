# 00 - Electron Platform Contract And Source Freeze

**Goal:** Before any porting starts, define the exact versions, artifact layout, bundle contract,
and success gates for a reusable Electron app platform on NanOS.

**Why this plan exists:** Without a platform contract, the team will accidentally build a MarkText
special case. This task freezes the interface that later Electron apps will use.

**Entry criteria:** none — this is the first plan. You need a host with `git`, `node` (any recent
LTS), `corepack`, `jq`, and `shasum`.

## Files

**Create:**
- `manifest/electron-stack.lock`
- `docs/en/electron-platform.md`
- `scripts/electron/README.md`

**Already exists (update only):**
- `docs/superpowers/plans/2026-07-07-electron-marktext/status.md`

**Modify later, after implementation exists:**
- `docs/en/writing-apps.md`
- `docs/superpowers/plans/README.md`

## Task 0.1: Pin Source Versions

- [ ] **Step 1: Clone MarkText and record the exact commit.**

```sh
mkdir -p "$SDK_WORK" && cd "$SDK_WORK"
git clone https://github.com/marktext/marktext marktext
cd marktext
git rev-parse HEAD            # -> the exact commit for the lock file
jq -r '.version, .packageManager, .engines.node, .devDependencies.electron' package.json
```

Expected (values may drift slightly; record what you actually see): `0.19.0`,
`pnpm@10.33.4`, `>=20.19.0`, `^42.1.0`.

- [ ] **Step 2: Resolve the floating Electron range to one exact version** using MarkText's own
  lockfile, not `npm view` (the lockfile is what MarkText CI actually uses):

```sh
cd "$SDK_WORK/marktext"
corepack enable && corepack prepare "$(jq -r .packageManager package.json)" --activate
pnpm install --frozen-lockfile --ignore-scripts    # --ignore-scripts: skip electron binary download
node -p "require('electron/package.json').version" # -> exact Electron version, e.g. 42.1.3
shasum -a 256 pnpm-lock.yaml                       # -> pnpm_lock_sha256
```

If `pnpm install --frozen-lockfile` fails because the lockfile is out of date, that is a finding:
record it, and pin to the newest MarkText commit where it succeeds.

- [ ] **Step 3: Extract Chromium and Node versions from the pinned Electron's DEPS.**

```sh
cd "$SDK_WORK"
git clone --depth 1 --branch "v<resolved-electron-version>" \
  https://github.com/electron/electron electron-pin
grep -E "'(chromium_version|node_version)':" electron-pin/DEPS
```

Expected output shape:

```text
    'chromium_version': '1xx.0.xxxx.xx',
    'node_version': 'v22.x.x',
```

Record both. These are the ONLY Chromium and Node versions this program builds.

- [ ] **Step 4: Write `manifest/electron-stack.lock`** with every value filled in — no angle
  brackets may survive:

```toml
[marktext]
repo = "https://github.com/marktext/marktext"
commit = "<exact commit from Step 1>"
version = "0.19.0"
package_manager = "pnpm@10.33.4"
node_engine = ">=20.19.0"
pnpm_lock_sha256 = "<from Step 2>"

[electron]
npm_range_seen = "^42.1.0"
version = "<resolved exact from Step 2>"
repo = "https://github.com/electron/electron"
tag = "v<resolved exact>"
chromium_version = "<from Step 3>"
node_version = "<from Step 3>"
```

- [ ] **Step 5: Commit** the lock file plus a status row (`M0 Source Freeze` → in progress).

## Task 0.2: Define Runtime And Bundle Layout

The layout must support many Electron apps sharing one runtime. Write all of this into
`docs/en/electron-platform.md` (section "Runtime And Bundle Layout"). These paths are the contract
consumed by plans 04–06; changing them later means updating those plans in the same commit.

- [ ] Runtime install layout (paths inside a running NanOS):

```text
/disks/main/nanos/lib/electron/
  electron.nxe
  resources/
  icudtl.dat
  locales/
  snapshot_blob.bin
  v8_context_snapshot.bin
```

- [ ] App layout:

```text
/disks/main/apps/<app>/
  <app>.nxe                 # tiny launcher that execs the shared runtime (see plan 05)
  app.asar                  # preferred app payload
  package.json
  resources/
  native/                   # NanOS-built native modules, if any
  nanos-app.toml            # NanOS-specific metadata
```

- [ ] `nanos-app.toml` schema (example values for MarkText):

```toml
name = "marktext"
display_name = "MarkText"
runtime = "electron"
main = "app.asar"
electron_min = "<resolved exact>"
argv = []
env = { ELECTRON_DISABLE_SECURITY_WARNINGS = "1" }
file_associations = ["md", "markdown", "txt"]
```

- [ ] Launch contract:
  - `/disks/main/bin/<app>.nxe` (link-farm entry) execs
    `/disks/main/nanos/lib/electron/electron.nxe` with the app directory as argument;
  - `open file.md` routes to the app through the existing associations mechanism —
    `/disks/main/nanos/config/associations.conf`, edited in Settings → Default Apps
    (see `docs/en/windowing.md`). The packager appends rows there at install time from
    `file_associations`;
  - from NanWM, app launch uses the existing `open`/launch socket path.

## Task 0.3: Define App-Port Boundary

Write into `docs/en/electron-platform.md` (section "App-Port Boundary"):

- [ ] Platform/runtime code may live only in Electron/Chromium/Node/NanOS platform directories
  (`ports/electron/`, `ports/chromium/`, `ports/node/`, kernel, libc-glue).
- [ ] App-specific patches must live under `ports/marktext/patches/`.
- [ ] The generic packager must work on a second tiny Electron app named `electron-notes-smoke`
  (built in plan 05) before MarkText is considered platform-clean.
- [ ] Native npm module policy (see also the `.ndl`/no-dlopen fact in EXECUTION_GUIDE):
  - required native modules must be rebuilt for `x86_64-nanos` and **statically registered** into
    the runtime (NanOS has no `.so` dlopen; plan 02 Task 2.5 defines the mechanism);
  - optional native modules may be disabled only if the app has an equivalent JS fallback;
  - every disabled optional module gets an entry in `ports/marktext/NANOS_NOTES.md`.

## Task 0.4: Define Final Acceptance Test

Create the exact final script contract now, even before it exists. Write it into
`docs/en/electron-platform.md` (section "Acceptance"):

```sh
scripts/smoke-marktext.sh
```

Expected behavior:

1. Build/install the MarkText bundle into `disk/image64.img`.
2. Boot NanOS with GUI enabled.
3. Launch MarkText from the desktop or command line.
4. Open `/disks/main/apps/marktext-smoke/input.md`.
5. Insert a line `NanOS Electron smoke OK`.
6. Save.
7. Quit MarkText.
8. Reopen the file with a simple CLI tool and assert the saved line is present.
9. Fail if QEMU logs any page fault, general protection fault, assert, or Electron fatal error.

## Task 0.5: Documentation Skeleton

- [ ] Create `docs/en/electron-platform.md` with these section headers (fill what this plan
  defines now; later plans fill the rest):
  - Source/Version Policy (point at `manifest/electron-stack.lock`);
  - Runtime And Bundle Layout (from Task 0.2);
  - App-Port Boundary (from Task 0.3);
  - Platform Gap Matrix (empty table, filled by plan 01);
  - How To Port Another Electron App (filled by plan 05);
  - Native Module Policy;
  - Known Unsupported Electron APIs (filled by plan 04);
  - Acceptance (from Task 0.4);
  - Debugging (filled by plan 03 subplan 05).
- [ ] Create `scripts/electron/README.md`: one paragraph saying these scripts are the
  Electron-stack build entry points and that big sources live in `$SDK_WORK` (never the repo).

## Gate

Run these; all must succeed:

```sh
test -f manifest/electron-stack.lock && ! grep -q '<' manifest/electron-stack.lock && echo LOCK-OK
test -f docs/en/electron-platform.md && echo DOC-OK
grep -q "Platform Gap Matrix" docs/en/electron-platform.md && echo MATRIX-STUB-OK
```

- [ ] `LOCK-OK`, `DOC-OK`, `MATRIX-STUB-OK` all print.
- [ ] A reviewer can explain from the doc alone how a second Electron app will be packaged without
  MarkText-specific code.
- [ ] `status.md`: mark `M0 Source Freeze` complete with a log row.
