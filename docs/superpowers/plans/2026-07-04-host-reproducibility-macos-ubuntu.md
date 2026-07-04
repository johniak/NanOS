# Host Reproducibility — macOS from zero + Ubuntu parity (build EVERYTHING on either)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fresh macOS machine OR a fresh Ubuntu machine goes from `git clone` to a booting NanOS — kernel, all ports, disk images, verify64, AND the GL desktop — using only versioned scripts and pinned sources. Nothing on this Mac is load-bearing anymore.

**Where this sits in the chain (do not duplicate the other plans):**
1. `2026-07-02-plan-1-repo-reorg-manifest-bootstrap.md` — versions the ~20 port recipes out of `~/Projects/nanos-sdk-work`, adds `manifest.toml` + `bootstrap.sh` + `nanos-fetch`. **Prerequisite, 0% done.**
2. `2026-07-02-plan-2-make-world-building-docs.md` — `make world`, BUILDING.md, the core-Linux path, remote-build/CI, clean-Ubuntu-VM gate for the NON-GL build. **Prerequisite, 0% done.**
3. `2026-07-04-gl-userspace-refactor.md` Tasks 1-2 — GL sources single-sourced in-repo, fork QEMU/virgl provenance pinned in `docs/en/gl-host-stack.md`. **Prerequisite for Tasks 3-5 here.**
4. **THIS plan** — the HOST layer those plans stop short of: scripted host setup on both OSes, the GL/GPU host stack reproducible on macOS and natively available on Ubuntu, per-OS run scripts + display oracle, and clean-machine gates that prove the whole chain including GL.

**Architecture:** Everything guest-side already builds in Docker containers (`nanos-build`, `nanos-sdk-dev`), so the OS-specific surface is exactly: host packages, QEMU (which QEMU + which display/accel flags), and the screenshot oracle. Key asymmetry this plan exploits: **the kosmickrisp ANGLE-Metal fork is a macOS-only workaround** (macOS has no native virglrenderer path); **on Ubuntu stock QEMU + distro virglrenderer drive the identical NanOS guest GL stack with zero forks** — and `/dev/kvm` replaces TCG, making everything (verify64, GL desktop, the i915 smoke gates) several times faster. Ubuntu is not a port target; it is the EASIER host.

**Tech Stack:** POSIX sh, Homebrew Brewfile, apt, docker, QEMU (brew / apt / kosmickrisp fork), swift (macOS capture only).

## Global Constraints

- No functional changes to the kernel, userland, or port build logic — host scripts, run-script switches, docs, and gates only.
- No Claude/AI attribution in commits.
- The existing macOS flow keeps working unchanged at every step (all additive; per-OS switches default to today's behaviour on Darwin).
- HANDS OFF `linuxkpi/`, `kext/`, `kernel/`, `external/` (the i915 session's territory).
- Anything downloaded gets a pinned version + SHA256 or a pinned git commit — no "latest".
- Every claim of "works on Ubuntu" is proven by an actual run recorded in the test-matrix doc (Task 5), not assumed.

---

## File Structure

**Created:**
- `scripts/hosts/Brewfile` — macOS host deps (docker, qemu, python, meson/ninja/cmake for the GL fork build, coreutils).
- `scripts/hosts/ubuntu-packages.txt` — apt list (docker.io, qemu-system-x86, qemu-utils, python3, git, ovmf).
- `scripts/hosts/host-setup.sh` — OS-detecting installer (brew bundle / apt-get) + docker image builds + `bootstrap.sh` invocation.
- `scripts/hosts/check-host.sh` — asserts every host dependency (per-OS), prints versions; consumed by run scripts and BUILDING.md.
- `scripts/hosts/build-gl-stack-macos.sh` — clones the pinned kosmickrisp QEMU + virglrenderer forks, applies the recorded patches, builds into `$(SDK_WORK)` (the executable form of `docs/en/gl-host-stack.md`).
- `scripts/hosts/qemu-flags.sh` — sourceable helper emitting per-OS QEMU binary + accel + display flags (see Task 2).
- `docs/superpowers/plans/2026-07-04-host-matrix.md` — the living support/test matrix (what was proven where, when, image commit).

**Modified:**
- `scripts/run64*.sh`, `scripts/qemu-verify.sh`, GL scripts (via `scripts/lib/gl-qemu-lib.sh` from the refactor plan) — consume `qemu-flags.sh`.
- `BUILDING.md` (from Plan 2) — gains the "GPU/GL desktop" per-OS section + the host-setup entry point.
- `docs/en/gl-host-stack.md` — gains the Ubuntu section.

---

### Task 1: Host dependency manifests + `host-setup.sh` + `check-host.sh`

One command prepares either OS. The manifest IS the documentation of what the host needs — no more tribal knowledge ("you need mako in a Dockerfile", "debugfs is keg-only").

**Files:** create the four `scripts/hosts/` files listed above (minus the GL builder); modify `BUILDING.md` intro to open with `sh scripts/hosts/host-setup.sh`.

**Contents (verify each against reality while writing — the lists below are the starting inventory, not gospel):**
- macOS Brewfile: `qemu` (plain, for non-GL run64), `python3`, `coreutils`, `meson`, `ninja`, `cmake`, `pkg-config`, `glslang` (fork build), cask `docker` (or document colima as the alternative). Xcode CLT assumed (checked, not installed).
- Ubuntu: `docker.io` (+ usermod docker group note), `qemu-system-x86`, `qemu-utils`, `python3`, `git`, `curl`. GL: `qemu-system-gui`, `libvirglrenderer1` (verify the distro QEMU has `virtio-gpu-gl` + `-display gtk,gl=on` compiled in — Ubuntu 24.04's does; assert it in check-host).
- `check-host.sh`: per-OS assertions — docker daemon reachable, images `nanos-build`/`nanos-sdk-dev` present (offer to build), QEMU version ≥ pinned floor, `/dev/kvm` accessible on Linux (warn-not-fail), GL stack present (delegates to `check-gl-host.sh` on macOS, `virtio-gpu-gl` device probe on Linux).

- [ ] **Step 1:** Write the manifests + `check-host.sh`; run on THIS Mac — all green (it must describe the machine that already works).
- [ ] **Step 2:** `host-setup.sh` (idempotent; re-run = no-op); dry-run mode printing what it would install.
- [ ] **Step 3: Commit** `git commit -m "hosts: dependency manifests + one-command host setup (macOS/Ubuntu)"`.

### Task 2: Per-OS QEMU selection — `qemu-flags.sh` + KVM

Today every run script hardcodes the macOS answer. Centralize: one sourceable helper answers "which QEMU binary, which accel, which display" so all scripts agree and Ubuntu support is one file.

**Interfaces:**

```sh
# scripts/hosts/qemu-flags.sh — source it, then use:
#   $NANOS_QEMU        - qemu binary (plain runs)
#   $NANOS_QEMU_GL     - qemu binary for GL runs (fork on Darwin, same as NANOS_QEMU on Linux)
#   $NANOS_ACCEL       - "-accel kvm" when /dev/kvm is usable, "-accel tcg" otherwise
#   $NANOS_DISPLAY     - "-display cocoa,zoom-to-fit=on" / "-display gtk" per OS
#   $NANOS_DISPLAY_GL  - "-display cocoa,gl=es,zoom-to-fit=on" / "-display gtk,gl=on"
#   $NANOS_DISPLAY_GL_HEADLESS - "-display egl-headless" (Linux; unset on Darwin)
# Overridable via env; defaults preserve today's macOS behaviour exactly.
```

- KVM caveat to encode: verify64's race gates (smptorture/netstorture) were calibrated on TCG; they already poll-for-verdict with ceilings (39ad3b9), so KVM only shortens them — but assert this by running the full verify64 under KVM once and recording it in the matrix.

- [ ] **Step 1:** Write the helper; convert `run64.sh`, `run64-gl.sh`, `qemu-verify.sh` (and the GL lib once the refactor's Task 7 lands — coordinate, don't block).
- [ ] **Step 2:** macOS regression: `make run64` + `make verify64` + a GL desktop boot behave exactly as before (same flags echoed).
- [ ] **Step 3: Commit** `git commit -m "scripts: per-OS QEMU/accel/display selection in one helper (KVM on Linux hosts)"`.

### Task 3: macOS GL host stack — from pinned recipe, by script

Turn `docs/en/gl-host-stack.md` (GL-refactor Task 2's provenance doc) into `build-gl-stack-macos.sh`: clone the two forks at pinned commits, apply the named patches, configure + build + install into `$(SDK_WORK)`, verify with `check-gl-host.sh`. The current fork builds become disposable artifacts.

- [ ] **Step 1:** Script it from the doc; every clone pinned (`git checkout <hash>`), every patch applied from a versioned `scripts/hosts/patches/` copy.
- [ ] **Step 2:** Prove it: build into a THROWAWAY prefix (`SDK_WORK=$(mktemp -d)/sdk`) on this Mac; boot the GL image on the freshly-built fork; screencapture oracle shows the desktop.
- [ ] **Step 3: Commit** `git commit -m "hosts: scripted kosmickrisp QEMU/virgl build from pinned sources (macOS GL stack reproducible)"`.

### Task 4: Ubuntu GL path — stock QEMU, same guest, headless oracle

The NanOS guest GL stack (LinuxKPI virtio-gpu + Mesa virgl) is host-agnostic; on Ubuntu it should light up on distro QEMU with zero forks. Prove it and make the oracle portable.

**Files:** modify `docs/en/gl-host-stack.md` (Ubuntu section), `scripts/gl-desktop-test.sh` (per-OS capture via the lib).

- [ ] **Step 1:** On an Ubuntu machine/VM with GPU or software GL (llvmpipe works for virgl host rendering — slower but correct): `make image64-gl` (docker) + boot with `$NANOS_QEMU_GL -device virtio-gpu-gl-pci -vga none $NANOS_DISPLAY_GL` → desktop renders. Record GL renderer string + result in the matrix.
- [ ] **Step 2:** Headless oracle: boot with `-display egl-headless` + QEMU monitor `screendump` → PNG of the desktop (no window manager needed — this is what CI will use). Wire as the Linux branch of the capture helper in `gl-qemu-lib.sh`.
- [ ] **Step 3:** Try `screendump` on macOS/kosmickrisp too — if the virgl readback path serves it, the oracle unifies and the Swift capture tool becomes macOS-interactive-only; record the answer either way.
- [ ] **Step 4: Commit** `git commit -m "gl: Ubuntu host path (stock QEMU virgl) + headless screendump oracle"`.

### Task 5: Clean-machine gates + the living support matrix

The proof. Plan 2 already defines a clean-Ubuntu-VM gate for the core build; extend the discipline to both OSes and to GL, and record results where they can't rot silently.

**Files:** create `docs/superpowers/plans/2026-07-04-host-matrix.md`; modify `BUILDING.md` (link + per-OS quickstart).

**Matrix rows (each: date, host, commit, result):** core `bootstrap → make world → run64 boot-to-shell`; `verify64`; `image64-gl` build; GL desktop visual; GL headless oracle. Columns: macOS (this Mac), macOS (clean `SDK_WORK` sandbox), Ubuntu VM (TCG), Ubuntu bare/KVM.

- [ ] **Step 1:** macOS clean-sandbox gate: `SDK_WORK=$(mktemp -d) NANOS_SDK=... sh scripts/hosts/host-setup.sh && make world && make verify64` — from the repo checkout only. Fix whatever it flushes out (that is the point).
- [ ] **Step 2:** Ubuntu gate: fresh VM (or the Plan-2 remote server) — same sequence + Task 4's GL steps. Under KVM, record verify64 wall-time next to the TCG number.
- [ ] **Step 3:** Fill the matrix with the real runs; BUILDING.md gets the two quickstart blocks (macOS/Ubuntu) pointing at `host-setup.sh`.
- [ ] **Step 4: Commit** `git commit -m "docs: host support matrix proven by clean-machine runs (macOS + Ubuntu, core + GL)"`.

---

## Execution order across the whole reproducibility effort

1. Plan 1 (repo-reorg) → 2. Plan 2 (make world) → 3. GL-refactor Tasks 1-2 → 4. this plan. Tasks 1-2 here can start in parallel with Plan 2 (no file overlap); Tasks 3-5 need the pins from the GL refactor.

## Explicitly NOT in this plan

- Windows/WSL2 (stays the one-liner in BUILDING.md per Plan 2's constraint).
- CI wiring (Plan 2 Task 7 owns it; this plan's headless GL oracle is what that CI will call).
- aarch64 hosts (Apple Silicon already runs everything via TCG x86_64 emulation — that IS this Mac; an aarch64 *guest* is out of scope entirely).
- Reproducibility of the i915/Dell flow beyond what `make world` covers (the Dell test protocol doc owns real-HW steps).

## Self-Review

- **No duplication:** host packages/QEMU/oracle are exactly the gaps Plans 1-2 left ("run64-gl.sh gets a Linux path" was a one-line aspiration in Plan 2 — this plan is that line, grown to a real deliverable). ✓
- **Additive on macOS:** every switch defaults to today's Darwin behaviour; regression steps in Tasks 2-3 enforce it. ✓
- **Every "works" claim has a gate:** Tasks 3-5 all end in a real boot + recorded matrix entry. ✓
- **Territory:** scripts/ + docs/ only; zero kernel/kext/linuxkpi contact. ✓
