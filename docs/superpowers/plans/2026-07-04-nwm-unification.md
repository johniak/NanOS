# One nwm — runtime GL detection, one image (kill the nwm/nwm-gl split)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** There is exactly ONE compositor binary and ONE disk image. At startup nwm probes the loaded display driver; if it supports OpenGL (virgl today, iris on the Dell tomorrow) the GPU backend drives the desktop, otherwise the CPU compositor does — same binary, same image, everywhere. The only thing that legitimately differs per host is WHICH QEMU runs it (plain vs GL-capable) — that split stays.

**Today's mess this removes:** `nwm.nxe` (126 KB, CPU-only, in-tree build) vs `nwm-gl.nxe` (14.7 MB, Mesa closure, SDK docker build) — installed by *different images* (`image64` vs `image64-gl`, the latter a debugfs byte-swap of the former); THREE run targets (`run64`, `run64-gl` — fork QEMU but the CPU image!, `run64-gl-desktop` — fork + swapped image); smokes and docs that must say which combination they mean. Runtime fallback ALREADY exists inside the GL build (`nw_gl_init` failure → CPU path), so the split is pure build/packaging debt, not an architectural need.

**Decision (overrides the note in `2026-07-04-gl-userspace-refactor.md`):** the shipped `nwm.nxe` IS the Mesa-linked build. Its runtime probe decides the backend. The image64/verify64 "stays Mesa-free" property is consciously traded away — `make world` (Plan 2) makes the Mesa closure reproducible, so the dependency is acceptable; a loud, explicit `NWM_CPU_ONLY=1` bootstrap escape remains for machines without the SDK closure.

**Ordering:** best executed AFTER `2026-07-04-gl-userspace-refactor.md` Task 1 (GL sources single-sourced, recipes in `ports/mesa-gl/`) — the Makefile work here then lands on versioned recipes. Task 1 below (probe hardening) has no dependency and can start immediately.

## Global Constraints

- HANDS OFF `linuxkpi/`, `kext/`, `kernel/`, `external/` (the parallel i915 session). The probe consumes `/dev/dri` as-is.
- Fork rules unchanged (single-buffered textures, no mid-frame FBO churn, fenced `glTexImage2D`).
- The CPU compositor's rendering behaviour does not change — only how the binary that contains it is built and chosen.
- Detection is **capability-based, not name-based**: try to bring the GL stack up and fall back on failure. Never a device-name whitelist (the same binary must light up on i915/iris untouched — that is the point).
- Every failure path must be BOUNDED in time and leave the CPU path fully working. A GL-less boot may not hang, and may not get slower by more than ~1 s.
- No Claude/AI attribution in commits.

---

## File Structure

**Modified:**
- `user/nwm/nw_compose_gl.c`, `user/glkms/glkms_init.c` — staged probe + clean unwind + one-line backend report.
- `Makefile` — `nwm` target becomes the Mesa-linked build (via `ports/mesa-gl/`); `image64` installs it; `image64-gl`, `IMAGE64_GL_*`, `run64-gl-desktop` DELETED; `run64-gl` = fork QEMU + the one image.
- `scripts/smoke-virtio-gpu.sh` (assert `backend=CPU`), `scripts/smoke-virtio-gpu-gl.sh` (assert `backend=GL`, use the one image), `scripts/run64-gl.sh`, `scripts/gl-desktop-*.sh` (image references).
- `docs/en/graphics.md`, `docs/en/gl-host-stack.md`, `docs/superpowers/plans/README.md`, the GL-refactor plan's "NOT in this plan" note.

---

### Task 1: Staged GL probe with a verdict line (no build changes yet)

Restructure init so every outcome is explicit, fast, and reversible. Stages, each with its own failure log + full teardown of everything the earlier stages created:

1. `open("/dev/dri/card0")` — `ENOENT` → CPU, silent (a VGA-only machine is not an error).
2. `drmGetVersion` → log the driver name.
3. KMS pick (connector+mode) — failure → CPU with reason.
4. GBM device + surface, `eglInitialize`/`eglCreateContext`/`eglMakeCurrent` — failure → CPU with the EGL error.
5. Shader/FBO init (existing `nw_gl_init` body) — failure → CPU.

Final single serial line either way: `nwm: display=<driver> backend=GL` or `nwm: display=<driver|none> backend=CPU (<stage>: <reason>)`. This line is the contract the smokes assert on.

**The dangerous case to prove:** virtio-gpu WITHOUT GL (plain `-device virtio-gpu-pci`, what `smoke-virtio-gpu` boots): `/dev/dri/card0` exists and KMS works, but there is no 3D capset — Mesa's virgl screen creation must fail CLEANLY here, not stall waiting on capset/fence answers. Measure the probe's wall time in this exact configuration; if anything blocks unboundedly, bound it at the glkms layer (this is the one place a timeout is honest: the capability probe).

- [ ] **Step 1:** Implement the staged unwind in `glkms_init.c` (today's `goto fail` path frees state but stages 2-5 aren't separable/logged) + the verdict line in `nw_compose_gl.c`/`nwm.c`.
- [ ] **Step 2:** Test all three matrix rows with the CURRENT nwm-gl build: (a) plain QEMU std VGA → `backend=CPU (no /dev/dri)`; (b) plain QEMU `-vga none -device virtio-gpu-pci` → `backend=CPU (egl: ...)`, probe < 1 s, desktop renders via CPU on the virtio scanout; (c) fork QEMU `virtio-gpu-gl-pci` → `backend=GL`, desktop renders GPU-composited (screencapture oracle).
- [ ] **Step 3: Commit** `git commit -m "nwm-gl: staged GL probe — capability-tested backend choice with a single verdict line"`.

### Task 2: One binary — `make nwm` builds the Mesa-linked compositor

**Files:** `Makefile` (the in-tree nwm compile is replaced; the `nwm-gl` target renamed/absorbed), `ports/mesa-gl/build-nwm-gl.sh` → `build-nwm.sh` (output name `nwm.nxe`).

- `make nwm`: requires the Mesa closure (`$(SDK_TC)/x86_64-nanos/lib/libgbm.a` etc.) — **fail loud** with `run 'make libdrm mesa' first (or NWM_CPU_ONLY=1 for a bootstrap build)` when absent.
- `NWM_CPU_ONLY=1 make nwm`: the old in-tree compile (no `-DNWM_GL`), still named `nwm.nxe`, prints a prominent `WARNING: bootstrap nwm without GL support` — exists so a fresh machine can reach a desktop before the ports are built; never the default; CI/world never uses it.
- Strip the binary in the recipe; record before/after size in the commit message (unstripped today: 14.7 MB).

- [ ] **Step 1:** Rename/rewire the recipe + Makefile; `make nwm` produces the Mesa-linked `bin/nwm.nxe`; `NWM_CPU_ONLY=1` produces the bootstrap one.
- [ ] **Step 2:** `make image64` installs it (no target changes needed — same artifact name); boot matrix rows (a) and (b) from Task 1 on this image.
- [ ] **Step 3: Commit** `git commit -m "build: one nwm — Mesa-linked compositor is THE nwm.nxe (NWM_CPU_ONLY bootstrap escape)"`.

### Task 3: One image — delete image64-gl and the third run target

**Files:** `Makefile` (delete `image64-gl`, `IMAGE64_GL_GRUB2*`, `run64-gl-desktop`; `run64-gl` keeps the fork-QEMU/resign logic, now booting `$(IMAGE64_GRUB2)`), `scripts/run64-gl.sh`, `scripts/gl-desktop-test.sh`, `scripts/gl-desktop-show.sh`, `scripts/smoke-virtio-gpu-gl.sh` (image paths), `docs/en/graphics.md`.

- Run-target semantics after this task: `run64` = plain QEMU (CPU backend via probe); `run64-gl` = fork QEMU, GL backend via probe, same image. Nothing else.
- `run64-gl-desktop` becomes a stub that prints `merged into run64-gl` and exits 1 for one release cycle (muscle memory), then dies.

- [ ] **Step 1:** Makefile + script surgery; `grep -rn "image64-gl\|nwm-gl" Makefile scripts/ docs/` returns only the deprecation stub and history notes.
- [ ] **Step 2:** `make image64 && make run64-gl` → GL desktop; `make run64` → CPU desktop; both from the SAME image file (assert md5 of the image used).
- [ ] **Step 3: Commit** `git commit -m "build: one image — image64-gl retired, run64-gl boots the unified image on the GL QEMU"`.

### Task 4: Gates on the unified world

- [ ] **Step 1:** Full `make verify64` on the unified image (every gate now boots the Mesa-linked nwm and exercises the CPU-fallback probe — this is the real regression net). Compare total wall time against the pre-unification baseline; the delta must be within noise (probe ≤ ~1 s per boot).
- [ ] **Step 2:** `smoke-virtio-gpu.sh` gains the assert `nwm: display=virtio_gpu backend=CPU` (non-GL device must fall back); `smoke-virtio-gpu-gl.sh` (host-gated, fork QEMU) asserts `backend=GL`. Wire the CPU-side assert into verify64.
- [ ] **Step 3:** Live-USB angle: the unified nwm is ~15 MB and the Dell boots per-sector USB-MSC — run the existing USB boot smoke and measure the exec-load time of nwm; if it is user-visible (> a few seconds), record the mitigation follow-on (GL backend as a lazily-loaded `.ndl` module — NanOS already has Windows-style DLLs) in the follow-ons section and in `docs/superpowers/plans/README.md`. Do NOT implement it now.
- [ ] **Step 4: Commit** `git commit -m "smoke: backend-verdict asserts; verify64 green on the unified nwm/image"`.

### Task 5: Docs + plan bookkeeping

- [ ] `docs/en/graphics.md`: one architecture note — single nwm, probe stages, verdict line, `NWM_NO_GL` (manual override) and `NWM_CPU_ONLY` (build escape) documented; `gl-host-stack.md` knob table updated.
- [ ] Amend `2026-07-04-gl-userspace-refactor.md`: the "Merging nwm.nxe/nwm-gl.nxe — intentionally NOT proposed" bullet gets a strike-through note pointing here (user decision 2026-07-04).
- [ ] Update `docs/superpowers/plans/README.md` (this plan's row + status).
- [ ] **Commit** `git commit -m "docs: unified nwm architecture + plan index"`.

---

## Follow-ons (recorded, not in scope)

- **GL backend as a `.ndl` plugin** — the "professional" endgame: slim nwm core + `nwgl.ndl` (Mesa closure) loaded only when the probe passes stage 2. Kills the 15 MB always-loaded cost AND restores a Mesa-free bootstrap path without a second binary. Non-trivial (Mesa static closure inside the .ndl loader, C++ TUs, relocations) — deserves its own plan once the loader's limits are checked. Trigger: if Task 4 Step 3 measures painful USB load times, this jumps the queue.
- **iris on the Dell**: no work needed here by design — the probe lights GL when `/dev/dri/card0` is i915 and the closure contains iris (i915 plan Task 9 adds `-Dgallium-drivers=virgl,iris`); add a matrix row to the Dell test protocol then.

## Self-Review

- **Answers the actual complaint:** one nwm, capability-probed at runtime; QEMU choice remains the only per-host difference; image count drops to one; run targets drop to two. ✓
- **The traded-away property is named** (Mesa-free image64) and the trade is justified + escaped (`NWM_CPU_ONLY`), not silently lost. ✓
- **The risky path (non-GL virtio-gpu probe) has an explicit bounded-time test** before any packaging changes land. ✓
- **No collision** with the i915 session (userspace/Makefile/scripts/docs only) and builds on, not into, the GL-refactor plan. ✓
