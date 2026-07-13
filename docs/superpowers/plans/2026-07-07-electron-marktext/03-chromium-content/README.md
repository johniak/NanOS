# Chromium Content Subplans — Execution Coordinator

Chromium is the hardest layer in the Electron/MarkText program. Do not execute it as one giant
task. This file is the protocol for executing the six subplans in this directory; the parent
coordinator is [../03-chromium-content.md](../03-chromium-content.md) and program-wide rules live
in [../EXECUTION_GUIDE.md](../EXECUTION_GUIDE.md) (read both before starting).

## Entry Criteria (check before subplan 00)

```sh
# run from the repo root
grep chromium_version manifest/electron-stack.lock
```

- [ ] `manifest/electron-stack.lock` pins `chromium_version` (plan 00 of the program).
- [ ] In QEMU: `epolltest`, `mmapexectest`, `shmdualtest` print PASS (plan 01 Tasks 1.2/1.3/1.5).
- [ ] `$SDK_WORK` has ≥ 100 GiB free (`df -h "$SDK_WORK"`) — a Chromium checkout + build is huge.
- [ ] A **native x86_64 Linux builder** is available for the Chromium build itself (16+ cores,
  64 GiB RAM, 250 GiB SSD, ccache) — see EXECUTION_GUIDE "Build hardware reality check".
  Emulated-Docker-on-Apple-Silicon builds are not viable at Chromium scale.

## Order And Gates

Execute strictly in order. Every subplan produces one concrete artifact and one gate command; run
the gate exactly as written in the subplan, then record a `status.md` row (house format in
EXECUTION_GUIDE) and tick the parent coordinator checklist.

| # | Subplan | Artifact | Gate (summary — exact command in the subplan) |
|---|---|---|---|
| 1 | [00-source-and-gn.md](00-source-and-gn.md) | pinned checkout + `scripts/electron/chromium-args-nanos.gn` | `gn gen out/NanOS` succeeds |
| 2 | [01-base-ipc-runtime.md](01-base-ipc-runtime.md) | Chromium `base` builds for NanOS | `ninja ... base` completes (or documented next blocker outside base) |
| 3 | [02-ozone-nanos.md](02-ozone-nanos.md) | `ui/ozone/platform/nanos/` backend | standalone Ozone window/pattern/input smoke on NanOS |
| 4 | [03-content-shell.md](03-content-shell.md) | `content_shell.nxe` + runtime data files | local HTML renders, JS mutates DOM |
| 5 | [04-network-storage-media.md](04-network-storage-media.md) | feature matrix in `docs/en/chromium-port.md` | file/http/font/storage smoke page fully green |
| 6 | [05-debugging-and-regression.md](05-debugging-and-regression.md) | `scripts/smoke-chromium-content.sh` + triage script | smoke passes AND fails correctly on bad input |

**Sanctioned parallelization:** subplans 02 (Ozone) and 03 (content shell) may run concurrently,
because upstream already ships `ozone_platform = "headless"` — content shell can render pages to
PNG files with **zero** display/input work. Subplan 03 defines a "headless first" Step 0 gate
that proves blink+V8+content on NanOS before (or while) the NanWM backend exists. The subplan 03
FINAL gate still requires the real `nanos` Ozone platform from subplan 02.

**If a gate does not pass, do not open the next subplan.** Add a FAIL row to `status.md`, fix the
failing layer (which may be a NanOS kernel/libc gap — follow the "How To Handle Missing NanOS
APIs" protocol in EXECUTION_GUIDE), rerun the same gate.

## Rules Local To The Chromium Layer

- **One GN args file.** `scripts/electron/chromium-args-nanos.gn` is the single source of truth;
  no subplan keeps its own variant. If an arg must change, change that file and note why in
  `ports/chromium/README.md`.
- **Patches are upstream-shaped.** Platform conditionals use the `nanos` build symbol
  (`is_nanos` in GN, `BUILDFLAG(IS_NANOS)`/`OS_NANOS` in C++), not edits sprayed through Linux
  code paths. Where reusing a Linux path is right, the conditional becomes
  `is_linux || is_nanos` — visible and greppable.
- **Patch bookkeeping.** Every patch in `ports/chromium/patches/` is numbered
  (`0001-...patch`), has the standard header (EXECUTION_GUIDE), and is applied by
  `scripts/electron/fetch-chromium.sh` — a clean re-fetch must reproduce the working tree.
- **Fix at the right layer.** A missing OS primitive gets a gap-matrix row + NanOS microtest +
  kernel/libc change (program plan 01 protocol) — never a Chromium-side workaround that fakes
  the primitive.
- **Scale warning.** Expect thousands of compile errors at first `ninja` invocations. Progress is
  measured by the classification ladder in subplan 01, not by error count. Work one error
  cluster at a time; commit after each cluster with the gate rerun.

## Definition Of Done For This Directory

All six subplan gates green, parent checklist ticked, `status.md` has `M3 Chromium Content`
checked, and the Electron runtime plan (../04-electron-runtime.md) is unblocked because its
entry criteria — content smoke + Ozone backend — exist.
