# 06 - MarkText Port And Acceptance App

**Goal:** Build MarkText for NanOS as an Electron app bundle and pass the final editor acceptance
test.

**Scope boundary:** This plan may patch MarkText, its packaging, and optional dependency handling.
It must not patch Electron/Chromium/Node runtime behavior — that belongs in the earlier platform
plans (if MarkText exposes a runtime bug, fix it there under its rules, with a test).

**Entry criteria:** the full "Review Checklist Before MarkText Work Starts" in EXECUTION_GUIDE is
green (lock, node smoke, chromium smoke, hello smoke, packager second-app smoke, clean runtime
patches). Dependency prep (Tasks 6.1–6.3) may start earlier in parallel with plan 04; packaging
and run gates may not.

## Files

**Create:**
- `ports/marktext/README.md`
- `ports/marktext/nanos-app.toml`
- `ports/marktext/patches/`
- `ports/marktext/NANOS_NOTES.md`
- `ports/marktext/dependency-audit.md`
- `scripts/electron/build-marktext.sh`
- `scripts/smoke-marktext.sh`
- `disk-content/marktext-smoke/input.md`

**Modify:**
- `Makefile` targets: `marktext`, `image64-electron` (plan 07 owns the final wiring).
- Associations: the packager's `--associations "md,markdown,txt"` flag feeds
  `/disks/main/nanos/config/associations.conf` (the real mechanism — see `docs/en/windowing.md`;
  there is no `config/etc/associations.conf`).

## Task 6.1: Source Prep And Dependency Freeze

- [ ] Use the MarkText commit pinned in `manifest/electron-stack.lock`
  (`$SDK_WORK/marktext` from plan 00 Task 0.1 — re-clone with
  `git checkout <commit>` if the dir was touched).
- [ ] Install with the pinned toolchain, frozen:

```sh
cd "$SDK_WORK/marktext"
corepack prepare "$(jq -r .packageManager package.json)" --activate
pnpm install --frozen-lockfile
shasum -a 256 pnpm-lock.yaml   # must equal pnpm_lock_sha256 in the lock file
```

- [ ] Create `ports/marktext/NANOS_NOTES.md`: exact upstream commit, exact Electron version,
  (later) disabled optional deps and NanOS patches.

## Task 6.2: MarkText JS Build On The Host

MarkText's JS build (webpack/vite per its version) runs on the host, never inside NanOS.

- [ ] **Inspect before running** — record the findings in `NANOS_NOTES.md`:

```sh
jq -r '.scripts' package.json          # find the build (not package) script
jq -r '.main' package.json             # declared entry point
ls electron-builder* 2>/dev/null       # electron-builder config = packaging we will NOT use
```

- [ ] Run the JS-bundle build script (the one that produces bundles WITHOUT invoking
  electron-builder's platform packaging — typically a `build` script that stops after webpack;
  record the exact name you used). If every path runs electron-builder, use
  `--dir` mode and harvest `resources/app.asar`-input directory only.
- [ ] Record the produced layout (main bundle, renderer bundle, preload, static assets) in
  `NANOS_NOTES.md`, and set `--entry` for the packager accordingly.
- [ ] Gate: a host script (`scripts/electron/build-marktext.sh`, which wraps install + build +
  verification) asserts the entry JS file exists and `grep`-finds the app name in it.

## Task 6.3: Optional And Native Dependencies

Audit from the lockfile, not from memory:

```sh
cd "$SDK_WORK/marktext"
jq -r '.dependencies // {}, .optionalDependencies // {} | keys[]' package.json
pnpm ls --depth 1 --json | jq -r '..|.name? // empty' | sort -u | head -100
find node_modules -name "*.node" -o -name "binding.gyp" | head -30   # native module inventory
```

- [ ] Write `ports/marktext/dependency-audit.md`: every native or external-binary dependency and
  its NanOS strategy (`static` | `disable` + JS fallback | `port binary`). Known candidates to
  check explicitly (verify against the actual tree, do not assume):
  - `native-keymap` — optional; disable if shortcuts still work through Electron for acceptance;
  - `@vscode/ripgrep` — external binary; if MarkText's in-folder search needs it at runtime,
    either port ripgrep via the SDK (it is Rust — check feasibility, record) or disable with a
    documented degraded-search note;
  - `keytar` — keychain; if unused by acceptance flows, disable with stub;
  - `chokidar`/watcher — polling fallback is already the platform default
    (`CHOKIDAR_USEPOLLING=1` from the launcher; verify MarkText's watcher honors it);
  - `fontmanager`/spellchecker natives if present — same treatment.
- [ ] Gate: `dependency-audit.md` complete; every `strategy = "disable"` entry has its JS
  fallback or feature-degradation note.

## Task 6.4: NanOS App Patches

Allowed patch classes (all in `ports/marktext/patches/`, standard header):

- [ ] disable auto-update UI and updater startup;
- [ ] route open-file/associations through NanOS paths;
- [ ] app data directory via `app.getPath('userData')` (already `$HOME/.config/marktext` from
  plan 04 — patch only if MarkText hardcodes elsewhere);
- [ ] disable unsupported OS integrations with visible no-op behavior (log line, not silence).

Forbidden patch classes:

- replacing the editor core; hardcoding the smoke file path; bypassing Electron APIs to talk to
  NanWM directly; silently deleting major features.

## Task 6.5: Package MarkText

- [ ] Through the generic packager only:

```sh
scripts/electron/package-app.sh \
  --name marktext --display-name "MarkText" \
  --source "$SDK_WORK/marktext" --entry "<from Task 6.2>" \
  --out "$SDK_WORK/electron-apps/marktext" \
  --associations "md,markdown,txt" \
  --native-modules ports/marktext/native-modules.toml
```

- [ ] Install to `/disks/main/apps/marktext/` + link-farm `/disks/main/bin/marktext.nxe`;
  associations rows appear in `/disks/main/nanos/config/associations.conf`.
- [ ] Gate: MarkText startup log identifies the app (window title or log line), even if the
  window is still imperfect.

## Task 6.6: MarkText Functional Smoke

`scripts/smoke-marktext.sh`, house style. Flow:

- [ ] Boot GUI image; launch via `marktext /disks/main/apps/marktext-smoke/input.md`
  (input.md staged from `disk-content/marktext-smoke/input.md`, content: a heading + one
  paragraph).
- [ ] Wait (bounded) for the editor window; inject keystrokes appending a new line
  `NanOS Electron smoke OK`; save with `Ctrl+S`; quit with `Ctrl+Q`/window close.
- [ ] Verify on disk from the guest shell:

```sh
grep "NanOS Electron smoke OK" /disks/main/apps/marktext-smoke/input.md && echo SAVE-OK
```

- [ ] Relaunch MarkText on the same file; assert the line is displayed (screenshot hash or a
  MarkText console log of the loaded document — pick the assertable one and record it).
- [ ] Idle-CPU check (plan 01 Task 1.6 promise): 30 s idle editor < 10% CPU.
- [ ] Automation escape hatch (acceptable first gate): env `MARKTEXT_NANOS_SMOKE=1` making
  MarkText itself run open→insert→save through its OWN command layer (a patch in
  `ports/marktext/patches/`, allowed class: test instrumentation; it must exercise the real
  editor commands, not `fs.writeFileSync`).

## Task 6.7: Manual Acceptance Checklist

Before marking success:

- [ ] Launch from desktop icon or Files app; launch from terminal with a file argument.
- [ ] Open existing markdown file; create new file; type; save; reopen.
- [ ] Preview renders heading, bold, code block, and link.
- [ ] Window close leaves no zombie processes (`ps` in guest).
- [ ] Idle MarkText does not consume runaway CPU.

## Stage Demo

Demo app: **MarkText itself** — the acceptance app is the demo. Demo scenario, using only
already-existing desktop pieces plus the new stack: stage this repo's
`docs/superpowers/plans/2026-07-07-electron-marktext/README.md` on the image as
`/disks/main/home/jan/Documents/electron-plan.md`, map `.md` → MarkText in
`/disks/main/nanos/config/associations.conf` (existing mechanism, see `docs/en/windowing.md`),
double-click the file in the Files app, edit a line, save, reopen — the OS editing its own plan.
Covered by `scripts/smoke-marktext.sh` + the Task 6.7 manual checklist (the association-launch
path is already its first item); take one screenshot for `docs/en/marktext.md`.

## Gate

- [ ] `make marktext` builds + packages reproducibly.
- [ ] Stage demo done: `.md` association opens MarkText from the Files app; screenshot saved for
  `docs/en/marktext.md`.
- [ ] `scripts/smoke-marktext.sh` green (and proven able to fail).
- [ ] Manual checklist all ticked.
- [ ] All MarkText-specific limitations documented in `ports/marktext/NANOS_NOTES.md`.
- [ ] `status.md`: `M6 MarkText Build` + `M7 MarkText Run` checked with log rows.
