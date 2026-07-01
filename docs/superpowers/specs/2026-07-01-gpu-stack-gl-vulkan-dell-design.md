# GPU Stack — OpenGL + Vulkan on QEMU/macOS and the Dell — Design Decision Record

> Status: **APPROVED** (supersedes `feat/gpu-accel-virgl`'s
> `2026-06-29-gpu-accel-desktop-virgl-design.md`, which targeted QEMU only and a bespoke
> `/dev/vgpu` ioctl surface).
> Date: 2026-07-01. Dev host: **MacBook (Apple Silicon M4), macOS 15+**. Target HW:
> **Dell Latitude 5310** (Comet Lake-U, Intel UHD Graphics Gen9.5).

## 1. Requirements (what changed vs the old spec)

1. GPU-accelerated desktop (nwm) **and an app-facing GL API**, not just compositing.
2. Everything testable on **QEMU on the M4** (x86_64 guest under TCG — no HVF for x86 guests).
3. The same stack must later run on the **real Dell** — the old spec explicitly scoped the
   Dell out; this one makes it a first-class target.
4. Vulkan is wanted, but as a later stage, not the foundation.
5. No shortcuts: this is foundation work (see memory: no-shortcuts-on-foundations).

## 2. Verified platform facts (2026-07 research)

- **virgl (OpenGL) on macOS/ARM: WORKS.** `virtio-gpu-gl` is accelerator-independent (works
  under TCG); virglrenderer renders via ANGLE→Metal. Homebrew's stock QEMU lacks it; the
  `startergo/homebrew-qemu-virgl-kosmickrisp` tap ships QEMU (incl. `qemu-system-x86_64`)
  with virglrenderer + ANGLE + KosmicKrisp.
- **venus (Vulkan) on macOS: newly possible, still risky.** virglrenderer now has Venus
  support on macOS via KosmicKrisp/MoltenVK (macOS 15+, Apple Silicon). But the guest needs
  a full Mesa venus driver + blob resources + hostmem mapping (venus serializes the whole
  Vulkan API — no minimal-port option), and all documented setups use KVM/HVF guests;
  venus + TCG x86_64 is unproven. → Vulkan is **stage 3**, gated on a stock-Linux-guest
  validation of the host stack first.
- **Dell (Gen9.5/CML): i915 via LinuxKPI is the only real path.** FreeBSD's drm-kmod proves
  the LinuxKPI approach scales to i915. Mitigating facts: integrated graphics ⇒ no TTM
  (GEM-shmem path, which our KPI already has); GuC/HuC are **optional** on Gen9 (execlists
  submission; firmware loader may return -ENOENT); DMC is optional. Hard new requirements:
  real MSI interrupts, kthread-backed workqueues, request_firmware, io_mapping, stolen
  memory + ACPI OpRegion discovery.
- Mesa **virgl** and **iris** Gallium drivers need **no LLVM**; both need libdrm, pthreads
  (ported), EGL/GBM. Vulkan later: **vn** (QEMU) / **ANV** (Dell) sit on the same DRM ABI.

## 3. Architecture (variant A — GL-first on a shared Mesa+DRM foundation)

```
apps / nwm GL backend (GL ES 2 + EGL + GBM)
        │
     Mesa Gallium  ── virgl driver (QEMU)          ── iris driver (Dell)
        │ libdrm (real DRM ioctls)
/dev/dri/card0 + /dev/dri/renderD128   ← NanOS CharDevice forwarding to drm_ioctl()
        │
LinuxKPI ── unmodified virtio_gpu (QEMU)           ── unmodified i915 (Dell)
        │
virtio-gpu-gl (virglrenderer/ANGLE/Metal on M4)     UHD 620 Gen9.5 hardware
```

The **only** parts that differ between QEMU and the Dell are the kernel driver and the Mesa
driver; the DRM ioctl ABI, libdrm, Mesa, EGL/GBM, the nwm GL backend, and every app are
shared. Vulkan (venus/ANV) later plugs into the same DRM nodes.

Key decision vs the old spec: **no bespoke `/dev/vgpu`**. We expose the real DRM ioctl ABI
from day one (the vendored driver's `virtgpu_ioctl.c` + DRM core `drm_ioctl()` already
compile in the kext). A raw-ioctl `drmtest.nxe` (with a tiny virgl command emitter) replaces
the old plan's "minimal emitter" as the kernel de-risk — but against the real ABI, so
nothing is thrown away.

## 4. Rollout — three plans

1. `2026-07-01-plan-1-gl-desktop-virgl-qemu.md` — OpenGL desktop + app GL on QEMU/M4 (DRM nodes,
   GEM mmap, libdrm + Mesa virgl ports, GBM/KMS present, nwm GL ES backend, smoke gates).
2. `2026-07-01-plan-2-vulkan-venus.md` — Vulkan on QEMU (host-stack validation gate, blob
   resources/hostmem, syncobj, Mesa vn, offscreen smoke; zink experiment; ANV interfaces
   noted for the Dell).
3. `2026-07-01-plan-3-gl-on-dell-i915-iris.md` — i915 through LinuxKPI in stages (KPI upgrades
   proven on QEMU first, KMS-first milestone with firmware-fb fallback, execbuf, Mesa iris,
   Dell test protocol).

## 5. Out of scope

- WebGL/browser accel, video decode (media engines), multi-GPU, external displays beyond
  the eDP panel (Dell dock/HDMI is a follow-on after eDP works).
- Discrete-GPU drivers (amdgpu/nouveau), TTM.
- Replacing the CPU compositor: it stays as the permanent runtime fallback everywhere.
