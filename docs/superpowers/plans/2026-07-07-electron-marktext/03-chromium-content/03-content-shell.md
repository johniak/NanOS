# 03 - Content Shell Bring-Up

**Goal:** Link and run a minimal Chromium content shell on NanOS.

**Entry criteria:** subplan 01 gate green (`base` builds). Steps 1–3 and the headless Step 0 gate
may run in parallel with subplan 02 (see the directory README, "Sanctioned parallelization");
Steps 4–5 and the final gate need subplan 02's `nanos` Ozone backend.

## Decisions

- **Target: upstream `content_shell`.** It is the canonical minimal content embedder and what
  Electron's own bring-up uses. Only if it will not link after honest effort do we fall back to a
  custom minimal embedder that still uses the content API (`content::ContentMainRunner` +
  `ShellBrowserContext` shape) — never a custom HTML renderer. The fallback needs a written
  blocker record first.
- **Launch flags are the NanOS runtime defaults** (later baked into the Electron launcher):
  `--ozone-platform=nanos --single-process --no-zygote --no-sandbox --disable-gpu
  --use-gl=disabled --enable-logging=stderr --v=0`.
- **Canonical smoke page path:** staged in repo at `disk-content/chromium-smoke/index.html`,
  installed in the image at `/disks/main/apps/chromium-smoke/index.html`. All later plans use
  this path.

## Files

**Create:**
- `scripts/electron/build-chromium-content.sh` — `ninja -C .../out/NanOS content_shell` + `.nxe`
  conversion + copy into `bin/`
- `disk-content/chromium-smoke/index.html`
- `docs/en/chromium-port.md` (first sections: build, run, flags)
- `Makefile` target `chromium-content` (host wrapper calling the build script)

## Steps

- [ ] **Step 0: Headless-first gate (the cheap proof that blink+V8 live on NanOS).** Build
  content shell with the upstream headless Ozone platform (add `ozone_platform_headless = true`
  alongside `nanos` in the canonical args — both can be compiled in; the runtime flag picks one)
  and screenshot the smoke page to a file, no window or input needed:

```sh
content_shell.nxe --ozone-platform=headless --single-process --no-zygote --no-sandbox \
  --disable-gpu --use-gl=disabled --run-web-tests=false \
  --screenshot=/tmp/smoke.png file:///disks/main/apps/chromium-smoke/index.html
```

  Gate: `/tmp/smoke.png` exists, is non-trivially sized, and the log shows
  `NANOS-SMOKE-JS-RAN`. Everything blocking this step is a content/blink/V8 problem — exactly
  the class of bug you want to fight WITHOUT display variables. (Exact screenshot flag name
  varies by milestone — `--screenshot` comes from headless mode; record the working invocation
  in `docs/en/chromium-port.md`.)

- [ ] **Step 1: Build.**

```sh
ninja -C "$SDK_WORK/chromium-src/src/out/NanOS" content_shell
```

Expect a new error wave (content/, blink/, v8/, skia/, net/). Same classification ladder and
commit discipline as subplan 01. This is the longest step of the whole Chromium plan; keep
`status.md` updated with the current blocking cluster at least daily.

- [ ] **Step 2: Runtime files.** Content shell needs data files next to the binary. Install into
  `/disks/main/nanos/lib/chromium/` (image staging rules):
  - `icudtl.dat` (if intl enabled at all — try `icu_use_data_file=true` default),
  - `content_shell.pak` (resources),
  - `snapshot_blob.bin` / `v8_context_snapshot.bin` if V8 snapshots are enabled (they may be off
    in the args file — then nothing to ship),
  - fonts: point Skia at the NanOS font dir (subplan 04 wires it; for this gate ASCII text with
    any renderable font is enough).
  Record every file + why in `docs/en/chromium-port.md`.

- [ ] **Step 3: Smoke page** at `disk-content/chromium-smoke/index.html`:

```html
<!doctype html>
<meta charset="utf-8">
<title>NanOS Chromium Smoke</title>
<h1 id="status">loading</h1>
<script>
  document.getElementById('status').textContent = 'Chromium on NanOS OK';
  console.log('NANOS-SMOKE-JS-RAN');   /* asserted from the serial/stderr log */
</script>
```

- [ ] **Step 4: Launch on NanOS** (QEMU `run64`, GUI):

```sh
content_shell.nxe --ozone-platform=nanos --single-process --no-zygote --no-sandbox \
  --disable-gpu --use-gl=disabled --enable-logging=stderr \
  file:///disks/main/apps/chromium-smoke/index.html
```

- [ ] **Step 5: Verify** (manually this subplan; scripted in subplan 05):
  - window shows `Chromium on NanOS OK` (JS mutated the DOM);
  - `NANOS-SMOKE-JS-RAN` appears in the log;
  - a keypress reaches the page (add a `keydown` listener logging to console if needed);
  - process exits cleanly on window close; `ps` shows no leftover processes;
  - QEMU `-d int` log has no faults.

## Gate

- [ ] `make chromium-content` produces `content_shell.nxe` reproducibly.
- [ ] Local HTML renders; JS runs (`NANOS-SMOKE-JS-RAN` in log); DOM text changed.
- [ ] Clean exit, no zombie processes, no faults in the QEMU log.
- [ ] `docs/en/chromium-port.md` documents build + run + runtime-file inventory.
- [ ] `status.md` row added; parent coordinator checkbox 03 ticked.
