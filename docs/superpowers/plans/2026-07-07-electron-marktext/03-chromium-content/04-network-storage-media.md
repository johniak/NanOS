# 04 - Chromium Network, Storage, Fonts, Locales, And Media Policy

**Goal:** Make Chromium content useful enough for Electron apps while explicitly disabling features
NanOS does not support yet.

**Entry criteria:** subplan 03 gate green (content shell renders the basic smoke page).

## Decisions

- **HTTPS: not a gate here.** Electron hello and MarkText run from local files; `https://` is
  verified opportunistically against the existing OpenSSL/cert stack and recorded either way in
  `docs/en/chromium-port.md`. `file://` and loopback `http://` ARE gates.
- **Fonts: Skia custom font manager over a fixed directory** — no fontconfig. Build with
  `skia_use_fontconfig = false` and point the `SkFontMgr` custom-directory backend at the NanOS
  font dir (the one found in program plan 01 Task 1.7). Default sans = IBM Plex Sans, mono =
  JetBrains Mono.
- **Storage paths:** profile/cache under `$HOME/.cache/chromium-content/` for content shell
  (`--data-path` flag); Electron apps later use `$HOME/.config/<app>` (plan 04). Temp under
  `/tmp`.
- **Media: audio/video capture and audio output disabled** via GN (`media_use_ffmpeg` stays ON
  for image/video decode; `use_alsa`/`use_pulseaudio` already false). A silent `<video>` decode
  is out of scope for M3; PNG/JPEG/SVG rendering is IN scope (MarkText preview needs images).
  Chromium must not spin retrying nonexistent device nodes — verify with the idle-CPU check.

## Steps

- [ ] **Step 1: Feature smoke page.** Replace/extend the staged page with
  `disk-content/chromium-smoke/features.html` (installed at
  `/disks/main/apps/chromium-smoke/features.html`):

```html
<!doctype html>
<meta charset="utf-8">
<title>NanOS Feature Smoke</title>
<style>
  body { font-family: sans-serif; }
  .mono { font-family: monospace; }
  #cssloaded { color: rgb(0, 128, 0); }
</style>
<h1 id="status">running</h1>
<p id="cssloaded">css-color-check</p>
<p class="mono">mono-font-check</p>
<p id="unicode">zażółć gęślą jaźń — ünïcode ✓</p>
<img id="img" src="smoke.png" width="32" height="32">
<script>
  const results = {};
  results.js = true;
  results.css = getComputedStyle(document.getElementById('cssloaded')).color === 'rgb(0, 128, 0)';
  try { localStorage.setItem('k', 'v'); results.storage = localStorage.getItem('k') === 'v'; }
  catch (e) { results.storage = false; }
  document.getElementById('img').onload  = () => { results.img = true;  finish(); };
  document.getElementById('img').onerror = () => { results.img = false; finish(); };
  function finish() {
    fetch('http://127.0.0.1:8099/ping').then(r => r.text())
      .then(t => { results.http = (t.trim() === 'pong'); done(); })
      .catch(() => { results.http = false; done(); });
  }
  function done() {
    const failed = Object.keys(results).filter(k => !results[k]);
    const verdict = failed.length ? 'NANOS-FEATURES-FAIL:' + failed.join(',')
                                  : 'NANOS-FEATURES-PASS';
    document.getElementById('status').textContent = verdict;
    console.log(verdict);
  }
</script>
```

  Add a 32×32 `smoke.png` next to it (generate once with `scripts/png2raw.py`'s inverse or any
  host tool; commit the binary).

- [ ] **Step 2: Loopback HTTP server.** NanOS already has an httpd path
  (`scripts/httpd-qemu.sh`, `disk-content/www`). Serve `pong` at `/ping` on port 8099 inside the
  guest for the smoke (document the exact command in `docs/en/chromium-port.md`).
- [ ] **Step 3: Fonts.** Wire the Skia custom font dir (Decisions) behind `is_nanos`; verify the
  unicode line renders (Polish diacritics above) — screenshot-diff against a missing-glyph box
  render is the check.
- [ ] **Step 4: Storage.** Verify `localStorage` survives a content-shell restart with the same
  `--data-path` (run smoke twice, second run reads the key before writing).
- [ ] **Step 5: Locales/ICU.** If intl is disabled in the args file, record the consequence
  (`Intl` object absent) in `docs/en/chromium-port.md`; MarkText gate in plan 06 decides whether
  small-icu must be turned on. If enabled, ship `icudtl.dat` and locale packs (subplan 03
  runtime-file rules).
- [ ] **Step 6: Idle CPU.** With the feature page open and idle for 30 s, CPU of the content
  shell process stays under 10% (read `/proc/<pid>/stat` utime deltas via the serial shell).
  A busy loop here usually means a device-probe retry loop — find it and disable the prober.
- [ ] **Step 7: Document the matrix** in `docs/en/chromium-port.md`: one table —
  feature | state (works/disabled/deferred) | evidence | flag.

## Gate

- [ ] Feature page prints `NANOS-FEATURES-PASS` (log-assertable) — that single marker covers
  file:// load, CSS, image decode, JS, localStorage, and loopback HTTP.
- [ ] localStorage persists across restart.
- [ ] Non-ASCII text renders with real glyphs.
- [ ] Idle CPU check passes.
- [ ] Feature matrix table exists in `docs/en/chromium-port.md` (including the disabled-media
  list and the HTTPS finding).
- [ ] `status.md` row added; parent coordinator checkbox 04 ticked.
