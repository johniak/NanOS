# GL Userspace Refactor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consolidate the OpenGL userspace stack (glkms + nw_compose_gl + the Mesa port flow + GL test tooling) from "bring-up quality" to "foundation quality" before the i915/iris work stands on it: one source of truth for every GL source file, versioned port recipes, GL backend honouring user settings, diagnostic code out of the hot path, a cheaper present, and shared ABI headers.

**Why now:** Plan 1 (virgl GL desktop) is complete and user-confirmed stable; the i915 plan (`2026-07-01-plan-3-gl-on-dell-i915-iris.md`) reuses this exact userspace unchanged, so every wart fixed here is fixed for the Dell too. The audit found real rot, not just style: `$(SDK_WORK)/mesa-port/gles2info.c` is a stale July-2 debug variant (TEMP `setenv(MESA_DEBUG)` spam) that `make gles2info` would ship today, the GL backend ignores the user's transparency/radius/blur settings, and a 16 MB diagnostic buffer is allocated from inside the per-window composite loop.

**Architecture:** Seven independent tasks, ordered so the source-of-truth fixes land first (they prevent further drift while the rest proceeds). No task changes rendering behaviour except Task 3 (which makes the GL desktop OBEY settings it previously ignored) and Task 5 (fewer ioctls per swap, same pixels).

**Tech Stack:** existing — C userspace, Mesa 24.2.8 static closure, libdrm/GBM/EGL/GLES2, docker `nanos-sdk-dev` cross builds, fork QEMU (kosmickrisp) for the visual oracle.

## Global Constraints

- **HANDS OFF the i915 session's territory:** a parallel session is implementing the i915 plan and has live commits + a dirty tree in `linuxkpi/`, `kext/`, `kernel/`, `external/linux-6.12/`. This plan must not modify ANY file under those directories. (The recorded `lkpi resv_test`/`lkpi shmem` telemetry quieting stays a follow-on for after that campaign merges — see Follow-ons.)
- **The fork rules are law** (from the shred/hang investigations — see `nw_compose_gl.c` header comments): blur textures+FBOs allocated and attached ONCE at init, never mid-frame; window/wallpaper/chrome uploads are `glTexImage2D` + `glFinish` (never `glTexSubImage2D`); window content textures stay SINGLE-buffered. No task may regress these.
- **No behaviour change to the CPU compositor** (`nw_compose.c` path) — verify64's image64 uses it; the host tests are the referee.
- Every task ends with: full `make image64` + `make verify64` green, and for GL-visible tasks `make image64-gl` + a fork-QEMU boot with the screencapture oracle (isolated window capture, enlarged crops — never a pixelated full-screen shot).
- Boot test images from a COPY of the disk image (the user's own QEMU may hold a write lock on `disk/image64-gl-grub2.img`).
- No Claude/AI attribution in commits.

---

## File Structure

**Created:**
- `ports/mesa-gl/` — the versioned Mesa/GL port recipes (moved from unversioned `$(SDK_WORK)/mesa-port/`): `build.sh`, `build-glkms.sh`, `build-gles2info.sh`, `build-nwm-gl.sh`, `cross-nanos64.ini`, `cc-wrap`, `cxx-wrap`, `Dockerfile.mako`, `README.md`.
- `docs/en/gl-host-stack.md` — fork QEMU/virglrenderer provenance + rebuild instructions; GL env-knob table.
- `scripts/check-gl-host.sh` — asserts the fork binaries exist, prints versions/commits.
- `user/nwm/nw_gl_debug.c`, `user/nwm/nw_gl_debug.h` — the NWM_GL_SOLID discriminator, out of the hot loop.
- `user/include/nanos/fbdev.h`, `user/include/nanos/input.h` — the shared fbdev/evdev ABI structs.
- `scripts/lib/gl-qemu-lib.sh` — shared boot/login/capture helpers for the GL scripts.

**Modified:**
- `Makefile` — `MESA_PORT` recipe paths → `ports/mesa-gl/` (scripts read from the repo mount; build dirs stay in `SDK_WORK`).
- `user/nwm/nw_compose_gl.c` — settings plumbing, debug extraction, telemetry gating.
- `user/nwm/nwm.c`, `user/nwm/nwm_core.h`, `user/nwm/nw_compose.c` — shared frame geometry + settings hook + header dedup.
- `user/glkms/glkms_init.c` — per-BO FB cache.
- `user/term/nterm.c` (and any other fbdev-struct copier `grep` finds) — include the shared headers.
- `user/libc-glue/` — `nx_stream_bridge.c` moves here from `user/gles2info/`.
- `scripts/capture-qemu-window.swift` — macOS 15 rewrite.
- `docs/en/graphics.md` — knob table pointer + updated build flow.

---

### Task 1: One source of truth for GL sources + versioned port recipes

The drift is live: `user/gles2info/gles2info.c` (clean, Jul 4) vs `$(SDK_WORK)/mesa-port/gles2info.c` (stale Jul 2 debug variant with `setenv("MESA_DEBUG"...)`) — and `build-gles2info.sh` compiles the STALE one. `build-nwm-gl.sh` already got this right (compiles from the `/work/nanos:ro` repo mount); extend that pattern to everything, and put the recipes themselves under version control.

**Files:**
- Create: `ports/mesa-gl/` (git `mv`-equivalent copy of the 8 recipe files from `$(SDK_WORK)/mesa-port/`, then edit)
- Modify: `Makefile` (targets `mesa`, `libdrm`-adjacent GL apps, `glkms`, `gles2info`, `nwm-gl`: mount `$(CURDIR)` ro like `nwm-gl` does; scripts referenced from `ports/mesa-gl/` — copied into the port dir at build time or invoked via the nanos mount)
- Delete (in `$(SDK_WORK)/mesa-port/`, after verification): `gles2info.c`, `glkms.c`, `glkms_init.c`, `glkms_init.h`, and the now-repo-owned recipe scripts (leave a README pointing at `ports/mesa-gl/`).

**Interfaces:**
- Produces: `ports/mesa-gl/build-gles2info.sh` etc. compiling ONLY from `/work/nanos/user/...` paths. `$(SDK_WORK)` keeps: Mesa/libdrm tarballs + extracted trees + build dirs + installed sysroot artifacts (they are caches, not sources).

- [ ] **Step 1:** Diff every GL source that exists in both places (`gles2info.c`, `glkms.c`, `glkms_init.c/h`, any nwm TU copies); confirm the repo copy is the desired one in each case (for `gles2info.c` it is — the port copy is the TEMP-debug variant). Record any port-side change worth keeping by porting it INTO the repo copy first.
- [ ] **Step 2:** Copy the recipe files into `ports/mesa-gl/`, rewrite their source paths to `/work/nanos/user/...`, commit (`ports: version the mesa-gl recipes (source of truth moves into the repo)`).
- [ ] **Step 3:** Point the Makefile targets at `ports/mesa-gl/` + add the repo mount to the `glkms`/`gles2info` docker runs. Rebuild all three apps; `gles2info.nxe` MUST change (debug spam gone) — boot the GL image copy, `gles2info` prints one clean `renderer=virgl` line.
- [ ] **Step 4:** Delete the stale port-side copies; rerun `make gles2info glkms nwm-gl image64-gl` from clean to prove nothing referenced them. `make verify64` green.
- [ ] **Step 5: Commit** `git commit -m "build: GL apps compile from the repo tree only; stale mesa-port source copies removed"`.

### Task 2: Pin + document the GL host stack (fork QEMU/virgl)

The visual oracle and the whole GL desktop depend on two locally-built forks (`$(SDK_WORK)/qemu-virgl-kosmickrisp`, virglrenderer fork with the borrow patch) whose provenance lives only in session memory. A laptop loss = losing the recipe.

**Files:**
- Create: `docs/en/gl-host-stack.md`, `scripts/check-gl-host.sh`
- Modify: `docs/en/graphics.md` (link), `scripts/run64-gl.sh` (call `check-gl-host.sh` first, fail with a pointer to the doc)

- [ ] **Step 1:** In the fork checkouts, record: origin URLs, branch, exact commit hashes (`git -C ... rev-parse HEAD`), local patch list (the macOS share-flag GAP1 patch, the borrow/Metal patch — name each and its purpose), and the exact configure/build commands used. Write `gl-host-stack.md` so a fresh machine can rebuild both forks + the run flags (`-device virtio-gpu-gl-pci -vga none -display cocoa,gl=es,zoom-to-fit=on`).
- [ ] **Step 2:** `check-gl-host.sh`: binaries exist + `--version` + stored-hash comparison, non-zero exit with the doc pointer otherwise. Wire into `run64-gl.sh`.
- [ ] **Step 3: Commit** `git commit -m "docs: pin the GL host stack (fork QEMU/virgl provenance + rebuild recipe)"`.

### Task 3: GL backend obeys the user's settings

Today `nw_compose_gl.c` hardcodes what the CPU path reads from `settings.yaml`: body alpha (`GL_WIN_ALPHA 206` / `GL_DARK_ALPHA 214` vs `nw_settings_win_alpha()`), border colour (`GL_BORDER_RGB`), blur spread (`STEP = 2.5f`), and the corner radius is only picked up at init (`nw_gl_set_radius` is never called from the `want_reload` path). The Settings app therefore silently does nothing on the GL desktop.

**Files:**
- Modify: `user/nwm/nw_compose_gl.c`, `user/nwm/nw_compose_gl.h`, `user/nwm/nwm.c` (apply_settings), `user/nwm/nwm_core.h` (shared frame geometry)

**Interfaces:**
- Produces (replaces `nw_gl_set_radius`):

```c
/* nw_compose_gl.h */
struct nw_gl_theme {
    int win_alpha, dark_alpha;   /* 0-255 body alpha (glass translucency)   */
    int radius;                  /* corner radius px                        */
    int blur_step_q8;            /* blur tap spread, 8.8 fixed (256 = 1px)  */
    uint32_t border_rgb;         /* 0xRRGGBB frame hairline                 */
    int glass_enabled;           /* transparency setting off -> opaque      */
};
void nw_gl_apply_theme(const struct nw_gl_theme *t);
```

- `nwm.c:apply_settings()` fills it from `g_set` (same derivations the CPU path uses: `nw_settings_win_alpha`, `nw_settings_blur_radius` mapped to a step, `g_set.transparency`) and calls `nw_gl_apply_theme` when `g_gl` — both at startup and on `want_reload`.
- Shared geometry: `frame_w`/`frame_h` are currently mirrored in both compose backends — hoist as `static inline nw_frame_w/nw_frame_h` into `nwm_core.h`, delete both copies.

- [ ] **Step 1:** Hoist the geometry helpers; host tests + full build prove no drift (`make test && make image64`).
- [ ] **Step 2:** Implement `nw_gl_apply_theme` (uniform values only — NO texture/FBO churn, the fork rules don't apply to uniforms); wire `apply_settings`. Remove the hardcoded macros.
- [ ] **Step 3:** On the fork QEMU: change transparency and corner radius in Settings → the GL desktop visibly updates WITHOUT restart (screencapture before/after, enlarged crop). Toggle transparency off → windows opaque (glass path skipped like `NWM_NO_GLASS`).
- [ ] **Step 4:** `make verify64` green (CPU path untouched — its settings flow didn't move).
- [ ] **Step 5: Commit** `git commit -m "nwm-gl: honour settings (alpha/radius/blur/border) live — theme uniforms, no hardcodes"`.

### Task 4: Diagnostic hygiene — debt from the shred/lag hunts out of the hot path

Three leftovers ship in the production compositor: (a) the `NWM_GL_SOLID` discriminator body sits INSIDE the per-window upload loop and lazily mallocs a 2048×2048 (16 MB) buffer there; (b) the 30-frame `nwm-gl: 30 frames | dirty...` telemetry prints to serial unconditionally; (c) the knobs are documented nowhere.

**Files:**
- Create: `user/nwm/nw_gl_debug.c`, `user/nwm/nw_gl_debug.h`
- Modify: `user/nwm/nw_compose_gl.c` (extract), `user/nwm/nwm.c` (telemetry gate), `docs/en/gl-host-stack.md` (knob table), `ports/mesa-gl/build-nwm-gl.sh` (add the new TU)

**Interfaces:**

```c
/* nw_gl_debug.h — GL diagnostics, all inert unless their knob is set. */
int  nw_gl_debug_solid_enabled(void);                    /* NWM_GL_SOLID / /nanos/nwm-solid, cached */
const uint32_t *nw_gl_debug_solid_px(int idx, int fw, int fh);  /* idx-keyed flat fill, exact-size buffer */
```

- The hot loop shrinks to: `const uint32_t *up_src = nw_gl_debug_solid_enabled() ? nw_gl_debug_solid_px(idx, fw, fh) : w->frame;` — one cached-flag branch, zero allocation when off.
- Telemetry: keep the counters, print only when `NWM_GL_STATS=1` (checked once). `NWM_GL_TRACE` (BLUR_MARK) stays as is — it is already gated and it is the documented oracle for fork hangs.
- Knob table in `gl-host-stack.md`: `NWM_NO_GL`, `NWM_NO_GLASS`, `NWM_GL_TRACE`, `NWM_GL_SOLID` + `/nanos/nwm-solid`, `NWM_GL_STATS`, `NWM_PROFILE` — one line each: what, when to use, cost.

- [ ] **Step 1:** Extract + gate; build `nwm-gl`.
- [ ] **Step 2:** Fork-QEMU boot: default serial has NO `nwm-gl: 30 frames` lines; `NWM_GL_STATS=1` restores them; `NWM_GL_SOLID=1` still paints the flat-colour windows (the discriminator must keep working — it earns its keep in every transport regression).
- [ ] **Step 3: Commit** `git commit -m "nwm-gl: hoist diagnostics out of the hot path; telemetry opt-in; knobs documented"`.

### Task 5: glkms present — per-BO framebuffer cache

`glkms_swap` currently does `drmModeAddFB` + `drmModeSetCrtc` + `drmModeRmFB` EVERY swap — creating and destroying a DRM framebuffer object per frame. GBM recycles 2-3 BOs, so cache the FB id on the BO once and steady-state becomes lock-front + SetCrtc. (True page-flip + vblank events need kernel/kext work in the i915 session's territory — recorded follow-on, NOT this task.)

**Files:**
- Modify: `user/glkms/glkms_init.c`, `user/glkms/glkms_init.h` (drop `front_fb` bookkeeping)

**Interfaces:**

```c
/* glkms_init.c — FB id cached on the BO; destroyed with the BO. */
struct glkms_fb { int fd; uint32_t id; };
static void fb_destroy(struct gbm_bo *bo, void *data)
{
    struct glkms_fb *f = data;
    if (f) { drmModeRmFB(f->fd, f->id); free(f); }
}
static uint32_t bo_fb_id(struct glkms *g, struct gbm_bo *bo)
{
    struct glkms_fb *f = gbm_bo_get_user_data(bo);
    if (f) return f->id;
    f = malloc(sizeof *f);
    if (!f) return 0;
    f->fd = g->fd;
    if (drmModeAddFB(g->fd, g->mode_w, g->mode_h, 24, 32,
                     gbm_bo_get_stride(bo), gbm_bo_get_handle(bo).u32, &f->id)) {
        free(f); return 0;
    }
    gbm_bo_set_user_data(bo, f, fb_destroy);
    return f->id;
}
```

- `glkms_swap`: `eglSwapBuffers` → `lock_front_buffer` → `bo_fb_id` → `SetCrtc` (only if the fb id differs from the last one set, else skip — same-buffer re-set is a no-op we can drop) → release the PREVIOUS front bo (no RmFB — the cache owns it). `glkms_close` releases the front bo; `gbm_surface_destroy` runs the destroy callbacks.

- [ ] **Step 1:** Implement; build `glkms.nxe` + `nwm-gl.nxe`.
- [ ] **Step 2:** Fork-QEMU: `glkms` gradient still fills the window; desktop session with a terminal flood + minutes of dragging — no visual regression, no `AddFB` errors on serial, memory steady (the BO set is bounded by GBM).
- [ ] **Step 3:** Check the `nwm-gl` frame telemetry (`NWM_GL_STATS=1`): dirty/drag averages same or better than the pre-task numbers (record both in the commit message).
- [ ] **Step 4: Commit** `git commit -m "glkms: cache DRM framebuffers per BO — AddFB/RmFB leave the swap hot path"`.

### Task 6: Shared userspace ABI headers + stream-bridge relocation

The fbdev structs (`fb_var`/`fb_fix` + FBIOGET ioctls) and evdev structs (`input_event`, `EV_*`, `REL_*`, `BTN_*`) are hand-copied per app ("mirrors nterm.c", "mirrors kext/mouse") — the exact drift class that produced the AT_* ABI bug. And `nx_bind_std_streams` (libc plumbing every Mesa-linked app needs) lives in `user/gles2info/`.

**Files:**
- Create: `user/include/nanos/fbdev.h`, `user/include/nanos/input.h` (contents = the structs/enums exactly as they are today, with a comment naming the kernel-side owner of each layout: `drivers/…` fb ioctl handler, `kext/mouse/MouseDevice.h`)
- Move: `user/gles2info/nx_stream_bridge.c` → `user/libc-glue/nx_stream_bridge.c`
- Modify: every copier `grep -rn "FBIOGET_VSCREENINFO\|struct input_event" user/ | grep -v include` finds (`nwm.c`, `nterm.c`, others as found) — delete local copies, include the headers; `Makefile` + `ports/mesa-gl/build-*.sh` (stream-bridge path).

- [ ] **Step 1:** Create headers; migrate one app (`nwm.c`), build.
- [ ] **Step 2:** Migrate the rest of the copiers found by grep; full `make image64` + `make verify64` (smoke-vt exercises nterm's fb path).
- [ ] **Step 3:** Move the stream bridge, fix the three GL app builds, rebuild all (`make gles2info glkms nwm-gl`).
- [ ] **Step 4: Commit** `git commit -m "user: shared fbdev/input ABI headers + stream bridge into libc-glue (kill per-app struct copies)"`.

### Task 7: GL test tooling — consolidate scripts, fix the capture tool

Seven GL scripts share copy-pasted QEMU boot/login/capture logic, `capture-qemu-window.swift` no longer compiles on macOS 15 (CGWindowListCreateImage removed), and bring-up one-offs linger.

**Files:**
- Create: `scripts/lib/gl-qemu-lib.sh` (boot-image-copy, wait-serial-marker, greeter login via `qemu-sendkey.py` [ctrl-alt-f7 first, then jan/jan], window-id lookup, isolated `screencapture -l`, cropped-enlarged artifact helper)
- Modify: `scripts/capture-qemu-window.swift` → macOS 15 rewrite (`CGWindowListCopyWindowInfo` for the window id, then shell out to `screencapture -x -l` — the approach proven in the debugging sessions), `scripts/run64-gl.sh`, `scripts/gl-desktop-test.sh`, `scripts/gl-desktop-show.sh`, `scripts/run-gles2info-gl.sh`, `scripts/smoke-virtio-gpu-gl.sh` (all consume the lib)
- Audit for deletion: `scripts/glpix-scanout-test.sh` (pre-Mesa raw-ioctl era — delete if `git log` confirms nothing references it and its oracle is covered by `drmtest` + `glkms`), `scripts/run64-gl-selftest.sh` (fold into `gl-desktop-test.sh` if redundant).

- [ ] **Step 1:** Write the lib by extracting the newest working logic (the flows debugged in the shred/lag sessions: monitor socket under `/tmp` [<104 byte path], `--type text --enter` sendkey flags, settle-sleeps after "GL compositor active").
- [ ] **Step 2:** Rewrite the capture tool; verify it compiles + captures the fork-QEMU window on this machine.
- [ ] **Step 3:** Convert the scripts one by one, running each after conversion (`gl-desktop-test.sh` must end with a real screenshot artifact).
- [ ] **Step 4:** Delete/fold the confirmed one-offs; note each deletion + its replacement in the commit message.
- [ ] **Step 5: Commit** `git commit -m "scripts: shared GL QEMU harness lib; macOS 15 window capture; prune bring-up one-offs"`.

---

## Explicitly NOT in this plan (recorded follow-ons)

- **Page-flip + vblank events** (`DrmDevice::read`, kext-side event injection): kernel/kext territory — coordinate with the i915 session; becomes valuable on real hardware (tearing on the Dell panel). Task 5's FB cache is the userspace half.
- **`lkpi resv_test`/`lkpi shmem` serial telemetry quieting** (`linuxkpi/kpi_fence.c`, `kpi_misc.c`): same territory conflict — do it right after the i915 Phase-A/B commits merge.
- **GL damage/scissor optimization** (present currently redraws the full scene on any dirty): a perf feature, not a refactor; measure on the Dell first — TCG numbers won't transfer.
- **Wallpaper cover-fit scaler → libnwui**: nice-to-have library extraction (`nwm.c:load_wallpaper` is generic bilinear scaling other apps could use); zero urgency.
- **Merging nwm.nxe / nwm-gl.nxe into one binary**: intentionally NOT proposed — the split keeps verify64's image64 free of the Mesa closure; the runtime CPU fallback already unifies behaviour.

## Self-Review

- **Territory check:** every modified path is under `user/`, `scripts/`, `ports/` (new), `docs/`, `Makefile` — zero overlap with the i915 session's `linuxkpi/`/`kext/`/`kernel/`/`external/` footprint. The Makefile is shared — its GL-app targets (lines ~408-520) are not touched by the i915 campaign (which adds `I915_OBJS` in the kext section); merge risk is a trivial textual one. ✓
- **Fork rules:** Task 3 changes uniforms only; Task 4 removes work from the loop; Task 5 touches DRM ioctls, not GL objects. No new texture/FBO lifecycle. ✓
- **Each task independently shippable and verifiable** with the standing oracles (verify64, host tests, screencapture). ✓
- **The drift bug is fixed by construction, not by discipline:** after Task 1 there IS no second copy to drift. ✓
