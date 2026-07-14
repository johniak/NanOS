# 05 - Generic Electron App Packager For NanOS

**Goal:** Create a reusable packaging path so future Electron apps can be built for NanOS without
forking the Electron runtime or writing custom Makefile blocks from scratch.

**Entry criteria:** plan 00 gate green (bundle contract exists). Tasks 5.1–5.4 can be developed
against the contract while plan 04 is still in flight; Task 5.5 (the proof) needs plan 04's
runtime working.

## Decisions

- **Launcher strategy: tiny C launcher per app** (option 1 of the old choose-one). NanOS already
  uses `.nxe` entrypoints and link-farm semantics; a symlink cannot carry per-app env/flags.
  Template below.
- **ASAR: yes.** `app.asar` is Electron-internal (no kernel work); native modules and large
  binaries go unpacked next to it in `resources/` when needed.
- **Example apps live in `examples/`** (`electron-hello` from plan 04, `electron-notes-smoke`
  here).

## Files

**Create:**
- `scripts/electron/package-app.sh`
- `scripts/electron/nanos-launcher.c` (template below)
- `scripts/smoke-electron-packager.sh`
- `docs/en/electron-apps.md`
- `examples/electron-notes-smoke/`

**Modify:**
- `Makefile`: targets `electron-notes-smoke`, generic packaging hook used later by `marktext`.

## Task 5.1: Packager Inputs And Outputs

- [ ] Command contract (exact flags):

```sh
scripts/electron/package-app.sh \
  --name marktext \
  --display-name "MarkText" \
  --source "$SDK_WORK/marktext" \
  --entry out/main/index.js \
  --out "$SDK_WORK/electron-apps/marktext" \
  [--associations "md,markdown,txt"] \
  [--native-modules native-modules.toml]
```

- [ ] Inputs: app directory with bundled JS, `package.json`, static assets, optional NanOS-built
  native modules.
- [ ] Outputs in `--out`: `app.asar` (packed with the `asar` npm tool on the host, pinned
  version), `nanos-app.toml` (generated from flags, schema from plan 00), compiled
  `<name>.nxe` launcher, `install-manifest.txt` (list of `source -> image path` lines consumed
  by the image staging rules).
- [ ] The script validates: entry file exists in the asar, name is `[a-z0-9-]+`, runtime version
  in `nanos-app.toml` matches the lock file. Fail loudly, no partial output.

## Task 5.2: Launcher

- [ ] `scripts/electron/nanos-launcher.c` — compiled per app with `-DAPP_NAME="..."` by the
  packager (SDK cc, then `.nxe` conversion):

```c
/* nanos-launcher.c — per-app Electron launcher: fixed runtime flags + app env, then exec the
 * shared runtime with this app's bundle directory. Compiled with -DAPP_NAME="<name>". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RUNTIME "/nanos/lib/electron/electron.nxe"
#define APP_DIR "/disks/main/apps/" APP_NAME

int main(int argc, char **argv) {
    setenv("CHOKIDAR_USEPOLLING", "1", 1);
    setenv("ELECTRON_DISABLE_SECURITY_WARNINGS", "1", 1);

    /* argv: launcher [files...] -> runtime <flags> APP_DIR [files...] */
    const char *fixed[] = {
        RUNTIME, APP_DIR,
        "--ozone-platform=nanos", "--single-process", "--no-zygote", "--no-sandbox",
        "--disable-gpu", "--use-gl=disabled", "--enable-logging=stderr",
    };
    int nfixed = sizeof fixed / sizeof fixed[0];
    const char **args = calloc(nfixed + argc, sizeof *args);
    for (int i = 0; i < nfixed; i++) args[i] = fixed[i];
    for (int i = 1; i < argc; i++) args[nfixed + i - 1] = argv[i];  /* pass files through */

    execv(RUNTIME, (char *const *) args);
    fprintf(stderr, "%s: exec %s failed\n", APP_NAME, RUNTIME);   /* only reached on failure */
    perror("execv");
    return 127;
}
```

- [ ] Startup errors land on stderr/serial; the launcher never silently exits.
- [ ] Gate: the packager-built launcher starts `electron-hello` (temporarily package hello with
  the packager to test — this also de-hand-rolls plan 04's packaging).

## Task 5.3: ASAR And Resource Handling

- [ ] Pack with pinned `asar` on the host; verify `require()` paths inside the asar resolve on
  NanOS (case sensitivity + `/` separators).
- [ ] Unpack rule: entries matching `--unpack` globs (native binaries) land in
  `resources/app.asar.unpacked/`, standard Electron convention.
- [ ] Gate: hello app loads HTML/CSS/JS from `app.asar` (delete the unpacked source from the
  bundle to prove it).

## Task 5.4: Native Module Hook

- [ ] Packager reads `native-modules.toml`:

```toml
[[module]]
name = "native-keymap"
required = false
strategy = "disable"   # or "static" (linked into the runtime via plan 02 Task 2.5)
reason = "optional MarkText accelerator enhancement; JS fallback works"
```

- [ ] `strategy = "static"`: packager errors unless the module is already in the runtime's
  linked-module registry (plan 02 mechanism) — it prints the exact rebuild command to add it.
- [ ] `strategy = "disable"`: packager writes a resolution stub (documented, visible warning at
  require-time), never a silent deletion.
- [ ] Gate: `electron-notes-smoke` carries one fake optional module with `strategy = "disable"`
  and packages cleanly with the warning stub in place.

## Task 5.5: Second App Proof

Create `examples/electron-notes-smoke/` — independent of MarkText:

`package.json`: `{ "name": "electron-notes-smoke", "version": "1.0.0", "main": "main.js" }`

`main.js`:

```js
const { app, BrowserWindow, ipcMain } = require('electron');
const fs = require('fs'), path = require('path'), os = require('os');
const NOTE = path.join(os.homedir(), 'Documents', 'note.txt');

app.whenReady().then(() => {
  const win = new BrowserWindow({ width: 640, height: 400,
    webPreferences: { preload: path.join(__dirname, 'preload.js') } });
  ipcMain.handle('save', (_e, text) => {
    fs.mkdirSync(path.dirname(NOTE), { recursive: true });
    fs.writeFileSync(NOTE, text);
    console.log('NANOS-NOTES-SAVED');
    return true;
  });
  ipcMain.handle('load', () => fs.existsSync(NOTE) ? fs.readFileSync(NOTE, 'utf8') : '');
  win.loadFile('index.html');
});
app.on('window-all-closed', () => app.quit());
```

`preload.js`:

```js
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('notes', {
  save: t => ipcRenderer.invoke('save', t),
  load: () => ipcRenderer.invoke('load'),
});
```

`index.html`:

```html
<!doctype html>
<meta charset="utf-8">
<title>notes-smoke</title>
<textarea id="t" rows="10" cols="60"></textarea><br>
<button id="save">save</button> <span id="status"></span>
<script>
  window.notes.load().then(v => {
    document.getElementById('t').value = v;
    if (v.includes('note-round-trip')) console.log('NANOS-NOTES-ROUNDTRIP');
  });
  document.getElementById('save').onclick = () =>
    window.notes.save(document.getElementById('t').value)
      .then(() => { document.getElementById('status').textContent = 'saved'; });
</script>
```

- [ ] Package with `package-app.sh`, install under `/disks/main/apps/electron-notes-smoke/`.
- [ ] `scripts/smoke-electron-packager.sh` (house style): run app, type `note-round-trip` into
  the textarea (QEMU sendkey), click/keyboard-trigger save, assert `NANOS-NOTES-SAVED`; relaunch
  the app, assert `NANOS-NOTES-ROUNDTRIP`. Clean exit, no faults. This gate is what prevents
  MarkText-specific platform shortcuts.

## Task 5.6: Documentation For Future Apps

- [ ] `docs/en/electron-apps.md`: prerequisites, pinning npm deps, bundling assets, native module
  policy, app data paths, file associations (the `associations.conf` mechanism), common failure
  modes (with triage-script pointers).
- [ ] Link from `docs/en/writing-apps.md`.

## Stage Demo

Demo app: **electron-notes-smoke** (Task 5.5) is this stage's demo — a real, tiny notes app a
user can launch from the desktop, type into, and whose note survives relaunch. No extra app.
`scripts/smoke-electron-packager.sh` is its test. For the milestone record, take one screenshot
of the reloaded note after app restart (the round-trip made visible) for `docs/en/electron-apps.md`.

## Gate

- [ ] `electron-hello` AND `electron-notes-smoke` both package with the generic packager.
- [ ] Stage demo screenshot (note reloaded after restart) saved for `docs/en/electron-apps.md`.
- [ ] `scripts/smoke-electron-packager.sh` green (and proven able to fail).
- [ ] No packager code mentions MarkText except in examples/docs.
- [ ] A junior can follow `docs/en/electron-apps.md` to package a third tiny app.
- [ ] `status.md`: `M5 Generic Packager` checked, log row added.
