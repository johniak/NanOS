# GPU-Accelerated Desktop (virtio-gpu / virgl) — Design Spec

> Status: **APPROVED — not yet implemented.**
> Builds directly on the LinuxKPI + virtio-gpu lift ([`2026-06-28-linuxkpi-virtio-gpu-design.md`](2026-06-28-linuxkpi-virtio-gpu-design.md)):
> the unmodified Linux 6.12 `virtio_gpu` DRM driver already renders the `nwm` desktop on
> `/dev/fb0` via 2D `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`. This spec adds **3D (virgl)
> GPU acceleration** to that same driver.
> Roadmap stream: **H — Linux Driver Compat (LinuxKPI)** (display/GPU), follow-on to the 2D bring-up.
> Date: 2026-06-29.

## 1. Goal & scope

Move the NanOS desktop from **all-CPU compositing** to **GPU compositing** over virtio-gpu's
3D (virgl) path, and in particular run the **glass blur on the GPU**.

Today `nwm` composites entirely on the CPU in `user/nwm/nw_compose.c`: a pre-rendered
gradient wallpaper, glass windows with anti-aliased rounded corners + per-window alpha, and a
**backdrop blur** built from downsample → small-blur → bilinear-upscale, wrapped in a
per-window blur cache, drag-slide reuse, and a per-frame rebuild budget. All of that caching /
budgeting machinery exists *only* to make CPU blur affordable. The final frame is written into
the `/dev/fb0` scanout buffer and the kext does a 2D transfer + flush each frame.

This work makes the GPU do that compositing — uploading each window's pixels as a texture and
drawing the scene (wallpaper, blurred backdrops, glass quads, window textures) directly into the
scanout 3D resource, with the blur as a **two-pass separable Gaussian fragment shader**.

### Anchored decisions

| Decision | Choice | Why |
|---|---|---|
| GPU API | **OpenGL via virgl** (not Vulkan/venus) | virgl is the mature virtio-gpu 3D path and runs on the QEMU-on-macOS dev host once QEMU is rebuilt with virglrenderer; venus needs host Vulkan (MoltenVK) + QEMU venus support, which is unavailable on macOS today, and has **no minimal-port option** (Venus serializes the entire Vulkan API, all-or-nothing). |
| Accel consumer (phase 1) | **The compositor only** | Highest impact per effort; apps stay software-rendered into shared buffers and the GPU composites them. Gets blur-on-GPU without an app-facing API. |
| Phase-1 guest userspace | **Minimal hand-written virgl emitter** | A few files + 2 TGSI shaders. De-risks the whole chain (QEMU tooling → kernel capset/context/submit/fence → present) without a Mesa port. It is also the bring-up vehicle Mesa needs in phase 2 — not throwaway. |
| Kernel↔user submission surface (phase 1) | **Bespoke `/dev/vgpu` ioctls** | The full DRM render-node ABI is deferred to phase 2 (Mesa needs it; the single-client compositor does not). Implemented by calling the unmodified driver's `virtgpu_vq.c` helpers, mirroring `virtio_gpu_present.c`. |
| Phase-1 blur | **Two-pass separable Gaussian fragment shader** | Replaces the CPU downsample/blur-cache/rebuild-budget machinery entirely on the GPU path. |
| Fallback | **Keep the CPU `nw_compose` path**, runtime-selected | Same `nwm` binary runs on the real Dell (firmware fb, no virtio-gpu) and on un-virgl'd QEMU. GPU backend is chosen only when a virgl-capable virtio-gpu is probed; any GPU submit error falls back to CPU for the session rather than crashing the desktop. |
| App-facing GL | **Phase 2: full Mesa Gallium virgl** | Recorded as a real phase below; detailed in its own spec/plan. |

**Success criterion (phase 1):** on a virgl-capable QEMU, the `nwm` desktop renders via the GPU
3D context — wallpaper, glass windows, and a GPU-computed backdrop blur — into the scanout 3D
resource, `screendump`-verified, with the CPU path untouched as the fallback when virgl is absent.

## 2. Architecture

A **GPU compositor backend** selected at runtime, with a clean user/kernel split:
**userspace builds virgl command buffers; the kernel ferries them to the device and manages
resources + fences.**

```
user/nwm
  └─ nw_compose            scene description (windows, wallpaper, glass, blur)
       ├─ CPU backend      existing raster path (fallback, unchanged)
       └─ GPU backend ───► libnwgl (minimal virgl emitter)
                              │  encodes virgl command buffers + 2 TGSI shaders
                              ▼  ioctls on
                           /dev/vgpu  (kext: kext/virtio_gpu)
                              │  PROBE / RESOURCE_CREATE_3D / TRANSFER_TO_HOST_3D / SUBMIT / PRESENT
                              ▼  calls the UNMODIFIED driver's helpers (virtgpu_vq.c)
                           virtio_gpu 3D context  ──► virtqueue ──► QEMU virtio-gpu-gl ──► virglrenderer ──► host GL
```

The scanout resource the desktop renders into is a **3D render target**; presenting it is the
driver's `RESOURCE_FLUSH` — no CPU readback of the composited frame.

## 3. Phase 1 — GPU compositor (minimal virgl emitter)

### K1 — virtio_gpu 3D enablement (`kext/virtio_gpu/`)
Negotiate `VIRTIO_GPU_F_VIRGL`, fetch the capset, and create a default 3D context at bringup,
reusing the unmodified driver's `virtgpu_vq.c` command helpers (same pattern as
`virtio_gpu_present.c`). When the feature is absent the kext leaves the existing 2D scanout path
in place and reports "no 3D" to userspace.

### K2 — `/dev/vgpu` submission surface (`kext/virtio_gpu/`)
A small char device with a bespoke ioctl set, each implemented by calling driver helpers:
- `VGPU_PROBE` / `VGPU_GET_CAPS` — is virgl available + the capset blob.
- `VGPU_RESOURCE_CREATE_3D` — create a 3D resource (texture / render target), return a handle.
- `VGPU_TRANSFER_TO_HOST_3D` — upload from a user buffer into a resource (window pixels, wallpaper).
- `VGPU_SUBMIT` — EXECBUFFER a virgl command buffer built in userspace.
- `VGPU_PRESENT` — bind the scanout resource + `RESOURCE_FLUSH`, with a fence wait.

*(The real DRM render-node ioctl ABI — `DRM_IOCTL_VIRTGPU_*`, which `virtgpu_ioctl.c` already
implements — is deliberately deferred to phase 2, where Mesa requires it.)*

### U1 — `libnwgl`, the minimal virgl emitter (`user/libnwgl/`)
A userspace library that:
- encodes virgl command buffers: create surface / sampler-view / shader / blend / vertex-elements,
  set framebuffer, `DRAW_VBO`;
- ships two **TGSI** shaders: a textured-quad passthrough, and a separable Gaussian blur;
- manages textures / render targets / the scanout target over K2;
- exposes a tiny scene API the compositor calls (upload-texture, draw-textured-quad, blur-region,
  present).

### U2 — `nw_compose` GPU backend (`user/nwm/`)
A backend behind a runtime switch:
- on damage, upload each window's pixel buffer to its GPU texture; wallpaper uploaded once;
- per frame, render into the scanout target: wallpaper quad → per glass window {separable-Gaussian
  blur of the backdrop region into an offscreen target → glass quad whose fragment shader applies
  rounded-corner + per-window alpha sampling the blurred target → the window texture on top};
- the CPU blur cache / rebuild budget are **not used** on the GPU path (the shader recomputes blur
  cheaply each frame);
- at startup, probe K2; if virgl is absent → run the existing CPU path unchanged. If a GPU submit
  errors mid-session → log and fall back to CPU for the session.

### H1 — virgl-capable QEMU on the dev host
The host QEMU (10.2.0, Homebrew) is currently built **without** virglrenderer (no
`virtio-gpu-gl-pci` device). Prerequisite: `brew install virglrenderer` + a QEMU built
`--enable-virglrenderer`, run with `-device virtio-vga-gl` / `virtio-gpu-gl-pci,gl=on`
(virglrenderer → host GL via ANGLE/Metal on macOS). Documented in the build notes and used by T2.

### Testing
- **T1 — host doctest of the virgl encoder:** byte-exact command buffers and shader blobs are pure
  data, host-testable with no QEMU (under the `nanos-test` image).
- **T2 — `smoke-virtio-gpu-gl` gate:** boot the virgl-capable QEMU, assert via serial that the 3D
  context came up, the desktop renders with a GPU-computed blur, a distinct-colours oracle on the
  `screendump`, and no kernel fault. Wired into `verify64` only when H1's QEMU is present (skipped
  with a logged notice otherwise, so the default `verify64` on a plain QEMU still passes).

## 4. Phase 2 — full Mesa Gallium virgl (app-facing GL ES)

A real, separately-planned phase (its own spec + implementation plan). Goal: expose **OpenGL ES**
to NanOS applications (and optionally retarget the compositor onto it), by porting:
- **Mesa Gallium + the virgl driver** (TGSI-based, so **no LLVM** needed) and the **GLSL→TGSI**
  compiler;
- the **real DRM render-node ioctl ABI** (`DRM_IOCTL_VIRTGPU_*`) on a `/dev/dri/renderD128` node,
  routed into the unmodified driver's existing `virtgpu_ioctl.c` handlers + GEM mmap;
- an **EGL + GBM winsys** over those ioctls;
- leveraging the existing NanOS **pthread** port for Mesa's threading.

Phase 1's kernel 3D path (capset / context / submit / fence) and the H1 QEMU tooling are exactly
what Mesa also depends on, so phase 1 is its de-risking bring-up vehicle. Phase 2 is recorded here
so the rollout is on the plan; its internals are out of scope for this spec.

## 5. Out of scope

- Vulkan / venus (host-unavailable on macOS; no minimal-port path) — see the API decision above.
- Multi-client GPU arbitration / a real DRM master + render-node multiplexing (phase 2+).
- Hardware-specific native GPU drivers (i915 etc.) — virtio-gpu only.
- Real-hardware (Dell) GPU accel — the Dell has no virtio-gpu; it stays on the firmware-fb CPU path.
