# 04 - Electron Runtime On NanOS

**Goal:** Build a reusable Electron runtime for NanOS: `electron.nxe` can launch a generic Electron
app bundle, render a window, run Node integration according to app settings, and support IPC.

**Entry criteria:** plan 03 fully green (`scripts/smoke-chromium-content.sh` passes) AND plan 02
fully green (`scripts/smoke-node.sh` passes). Both are hard prerequisites — Electron debugging on
top of an unproven Chromium or Node is forbidden by Rule Zero.

## Decisions

- **Electron version:** exactly `[electron].version` from `manifest/electron-stack.lock`; source
  fetched via Electron's own build flow (`electron/build-tools` or gclient with Electron's DEPS —
  whichever the pinned release documents) into `$SDK_WORK/electron-src`, reusing the SAME
  Chromium checkout/patches proven by plan 03 (Electron's DEPS pins that Chromium; do not fetch
  a second copy — configure the gclient solution to use the existing one).
- **Runtime defaults on NanOS** (baked into the launcher, plan 05): `--ozone-platform=nanos
  --single-process --no-zygote --no-sandbox --disable-gpu --use-gl=disabled
  --enable-logging=stderr`, env `CHOKIDAR_USEPOLLING=1`,
  `ELECTRON_DISABLE_SECURITY_WARNINGS=1`.
- **`userData` path:** `$HOME/.config/<app>` (desktop already runs apps as user `jan` with a
  real `$HOME` — see the greeter/HOME fact in the repo memory; per-user files never go under
  root-owned paths).
- **autoUpdater: disabled** for NanOS packages (no package manager). **globalShortcut:
  documented unsupported** for M4 (NanWM mapping is a follow-up).

## Files

**Create:**
- `scripts/electron/fetch-electron.sh`
- `scripts/electron/build-electron.sh`
- `scripts/smoke-electron-hello.sh`
- `examples/electron-hello/` (full app below)
- `docs/en/electron-runtime.md`
- `ports/electron/patches/`

**Modify:**
- `Makefile` targets: `electron-runtime`, `electron-hello` (host wrappers, SDK-container pattern).

## Task 4.1: Build Electron Against NanOS Chromium/Node

- [ ] `fetch-electron.sh`: pinned checkout into `$SDK_WORK/electron-src`, wired to the existing
  Chromium tree, applies `ports/electron/patches/*` (numbered, standard header). Idempotent.
- [ ] Electron GN args = the canonical `chromium-args-nanos.gn` plus Electron's own required args
  (`import("//electron/build/args/release.gn")`-shape for the pinned version). Keep the delta in
  ONE file: `scripts/electron/electron-args-nanos.gn`, which imports/appends the canonical one.
- [ ] Build:

```sh
ninja -C "$SDK_WORK/electron-src/src/out/NanOS" electron
```

- [ ] Same classification ladder as plan 03 subplan 01; Electron-layer errors usually cluster in
  `shell/browser/*` Linux integrations (DBus/GTK bits) — disable via supported flags or
  `is_nanos` conditionals; patch bookkeeping identical.
- [ ] Gate: `electron` target links; binary converts to `electron.nxe`.

## Task 4.2: Runtime Data Installation

- [ ] Inventory what the built Electron needs at runtime: `resources/default_app.asar` (or
  disable the default app), `icudtl.dat` (if intl on), locale packs, V8 snapshots if enabled,
  `LICENSES.chromium.html` (ship it — licensing).
- [ ] Image staging rules install everything under `/disks/main/nanos/lib/electron/` (contract
  layout from plan 00 Task 0.2).
- [ ] `electron.nxe` must locate resources relative to its own install path
  (`/proc/self/exe` readlink → dirname), never host paths. Patch if the pinned version assumes
  otherwise.
- [ ] Gate, inside NanOS:

```sh
/nanos/lib/electron/electron.nxe --version
```

Expected: `v<pinned version>` and no missing-file complaints in the log.

## Task 4.3: Generic Hello App

Create `examples/electron-hello/` with exactly these four files:

`package.json`:

```json
{
  "name": "electron-hello",
  "version": "1.0.0",
  "main": "main.js"
}
```

`main.js`:

```js
const { app, BrowserWindow, ipcMain } = require('electron');
const fs = require('fs');

app.whenReady().then(() => {
  const win = new BrowserWindow({
    width: 640, height: 400,
    webPreferences: { preload: require('path').join(__dirname, 'preload.js') }
  });
  ipcMain.handle('ping', () => {
    console.log('NANOS-HELLO-IPC-PING');
    fs.writeFileSync('/tmp/electron-hello.txt', 'Electron on NanOS OK\n');
    return 'pong';
  });
  win.loadFile('index.html');
});
app.on('window-all-closed', () => app.quit());
```

`preload.js`:

```js
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('nanos', { ping: () => ipcRenderer.invoke('ping') });
```

`index.html`:

```html
<!doctype html>
<meta charset="utf-8">
<title>electron-hello</title>
<h1 id="status">loading</h1>
<script>
  window.nanos.ping().then(r => {
    document.getElementById('status').textContent =
      r === 'pong' ? 'Electron on NanOS OK' : 'IPC BROKEN: ' + r;
    console.log(r === 'pong' ? 'NANOS-HELLO-PASS' : 'NANOS-HELLO-FAIL');
  });
</script>
```

- [ ] Package it with the plan-00 layout (hand-rolled for now — the generic packager arrives in
  plan 05; packaging by hand here is allowed and expected), install under
  `/disks/main/apps/electron-hello/`.
- [ ] Write `scripts/smoke-electron-hello.sh` (house style, clone the plan-03 subplan-05 skeleton;
  launch line runs the launcher/runtime against the app dir). Assertions:
  - `NANOS-HELLO-IPC-PING` in log (main process handled IPC);
  - `NANOS-HELLO-PASS` in log (renderer got the reply);
  - `/tmp/electron-hello.txt` content check via a typed `cat` after app quit;
  - clean exit, no faults, no zombies.
- [ ] Gate: `scripts/smoke-electron-hello.sh` passes and has been proven able to fail.

## Task 4.4: Electron App API Surface

Exercise enough Electron APIs for MarkText and future apps. For each: works → note it; broken →
fix at the right layer; unsupportable now → document in `docs/en/electron-platform.md` "Known
Unsupported Electron APIs" with the runtime behavior (silent no-op forbidden — log a warning).

- [ ] `app.getPath('userData')` → `$HOME/.config/<app>` (Decisions).
- [ ] `BrowserWindow` basics: title, resize, close events.
- [ ] `dialog.showOpenDialog`/`showSaveDialog` → back with libnwui's file dialog (the toolkit has
  one — Notepad uses it) via a small IPC shim, or document modal fallback for M4 if the shim is
  bigger than expected. MarkText needs open/save dialogs by plan 06 — decide here, record.
- [ ] `Menu` → app-local menus render in-content for M4 (NanWM global menu mapping is a
  follow-up).
- [ ] `clipboard` → NanWM clipboard if a NanWM clipboard API exists; else in-process only,
  documented.
- [ ] `shell.openPath` → the existing `open` + associations mechanism.
- [ ] `nativeTheme` → stable values from NanOS settings (light default).
- [ ] `globalShortcut`, `autoUpdater` → per Decisions.

## Task 4.5: Logging And Crash Diagnostics

- [ ] Electron stderr → serial + `/tmp/electron/<app>.log` (launcher redirection).
- [ ] Fatal crash prints a readable report on serial (NanOS fault dump is acceptable).
- [ ] Gate: a deliberate `process.crash()` in a test app produces a readable artifact and does
  not wedge the desktop (NanWM keeps running, window disappears).

## Gate

- [ ] `electron.nxe --version` works under NanOS.
- [ ] `scripts/smoke-electron-hello.sh` green (window + IPC + file write + clean exit).
- [ ] Runtime install is shared and app-independent; no MarkText references anywhere in
  `ports/electron/`.
- [ ] `docs/en/electron-runtime.md` has build/run/debug commands; unsupported APIs documented.
- [ ] `status.md`: `M4 Electron Runtime` checked, log row added.
