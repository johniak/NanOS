# NanOS Graphics & the LinuxKPI virtio-gpu driver

This document covers where the **display** comes from: how `/dev/fb0` is backed, and the
**LinuxKPI** layer that runs an **unmodified Linux GPU driver** to drive it. For the compositor and
GUI toolkit that *draw* into `/dev/fb0` see [windowing.md](windowing.md); for the loadable-module
mechanism see [kext.md](kext.md); for the VT multiplexing of the console see [x86_64.md](x86_64.md) §5.

---

## 1. Two sources of `/dev/fb0`

NanOS draws everything (the fbcon text consoles, then the `nwm` desktop) into a single linear
framebuffer exposed as `/dev/fb0`. That framebuffer can be backed two ways:

| Source | When | How |
|---|---|---|
| **Firmware framebuffer** | the default — real hardware (UEFI GOP) **and** QEMU's default VGA/VBE | the bootloader (Limine / GRUB multiboot) hands the kernel a linear framebuffer; `Kernel::start` builds the VT stack + `/dev/fb0` over it |
| **LinuxKPI virtio_gpu** | QEMU `-device virtio-gpu-pci` (esp. with `-vga none`) | the `virtio_gpu.nkext` runs the unmodified Linux DRM driver, creates a scanout, and bridges its buffer to `/dev/fb0` |

The two never collide: the virtio_gpu kext **only** takes over `/dev/fb0` when it actually finds a
virtio-gpu PCI device. On a machine without one (e.g. the real Dell — Intel graphics, no
`1AF4:1050`) it self-disables and the firmware framebuffer is used, exactly as before
(see §5).

---

## 2. The firmware framebuffer (default path)

The bootloader provides a linear framebuffer (address, pitch, width, height, bpp) in the boot info.
`bootinfo_x86_64.cpp` parses it; if present, `Kernel::start` brings up the VT graphics stack
(`VtManager` + `/dev/tty1..7`) and `/dev/fb0` (`Fb0Device`) directly over that memory. This is the
path on the real Dell (UEFI GOP) and on a plain `make run64` (QEMU std-VGA). 32-bpp BGRX. No driver
is involved — the firmware already set the mode.

---

## 3. The unmodified Linux virtio_gpu driver on LinuxKPI

The display is driven by the **unmodified Linux 6.12 `virtio_gpu` DRM driver (14 files) + DRM/KMS
core (~59 files) + virtio core**, compiled against the **LinuxKPI** shim and linked into one
`virtio_gpu.nkext` with no edits to the upstream source. LinuxKPI is the reusable, driver-agnostic
layer — *what it is, the full KPI surface, and the cooperative-UP simplifications are documented in
[linuxkpi.md](linuxkpi.md)*. This section covers only what is **specific to virtio-gpu**.

```
                       virtio_gpu.nkext
   ┌───────────────────────────────────────────────────────────┐
   │  UNMODIFIED Linux 6.12 source (vendored under external/)    │
   │   drivers/gpu/drm/virtio/*  (the virtio_gpu DRM driver)     │
   │   drivers/gpu/drm/*         (DRM/KMS core, atomic, gem_shmem)│
   │   drivers/virtio/*          (virtio_ring, virtio_pci_modern)│
   ├───────────────────────────────────────────────────────────┤
   │  LinuxKPI shim (linuxkpi/)  — linux/*.h + kpi_*.c           │
   │   slab, idr, dma-fence, scatterlist, shmem page provider,   │
   │   workqueue (synchronous), printk (%pV/%ps), pci/mmio/dma   │
   ├───────────────────────────────────────────────────────────┤
   │  NanOS glue (kext/virtio_gpu/)                              │
   │   virtio_transport.c   modern virtio-pci transport          │
   │   virtio_gpu_drv_entry.c  bootstrap: register + probe       │
   │   virtio_gpu_present.c    scanout + /dev/fb0 bridge         │
   └───────────────────────────────────────────────────────────┘
                 │ knx_* ABI            │ knx_fb_set_backing
                 ▼                      ▼
            NanOS kernel  ───────►  /dev/fb0  ◄──── nwm / fbcon draw here
```

### 3.1 Bring-up (`virtio_gpu_drv_entry.c`)

`loadAllKexts` runs the kext's `nkext_init()` at boot (before the scheduler). It:

1. initialises the DRM core (`drm_core_init`) and triggers the driver's own registration (the
   unmodified `module_virtio_driver()` expands, via a shim macro, to a callable init);
2. finds the virtio-gpu PCI function (`1AF4:1050`) — **if absent it returns immediately, a no-op**;
3. builds a `virtio_device` for it (the hand-written modern transport, our stand-in for
   `virtio_pci_common.c`) and replicates `virtio_dev_probe`'s feature negotiation;
4. calls the **real** `virtio_gpu_probe()` — which brings up the DRM device, GEM, fences, the
   connector (EDID → 1280×800), and KMS;
5. calls `virtio_gpu_fbcon_bringup()` to put it on screen.

### 3.2 The interrupt model (cooperative, INTx masked)

`loadAllKexts` runs **before the scheduler**, single-threaded and non-preemptible. virtio's INTx is
**level-triggered**: QEMU asserts it on the first used-buffer completion, and with no interrupt
handler wired the line stays high and storms the CPU forever — wedging boot. So the transport
**masks INTx** (`PCI_COMMAND.INTX_DISABLE`); virtqueue completions are instead harvested by a
**cooperative pump** — when DRM/virtio code waits on a fence or a vq response, the wait spins and
calls `vt_interrupt()`, which reads the ISR register and runs the ring callback. This is enough to
service the GPU command stream during boot.

### 3.3 The scanout + `/dev/fb0` bridge (`virtio_gpu_present.c`)

`virtio_gpu_fbcon_bringup()` drives scanout 0 entirely through the **driver's own command layer**:

1. `virtio_gpu_object_create()` — the driver allocates a shmem GEM buffer and issues the real
   `RESOURCE_CREATE_2D` + `RESOURCE_ATTACH_BACKING`;
2. `virtio_gpu_cmd_set_scanout()` — bind that resource to scanout 0;
3. `knx_fb_set_backing()` — expose the buffer as `/dev/fb0` and register a present callback;
4. each frame the present thread issues `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` so whatever fbcon /
   `nwm` drew is scanned out.

One enabling detail: the shim's **shmem page provider backs each GEM object with a single
physically-contiguous block** (`linuxkpi/kpi_misc.c`), so `page_address(pages[0])` is a flat
framebuffer the CPU and the device share (RAM is identity-mapped, so contiguous-virtual ==
contiguous-physical; the shim has no `vmap` that stitches scattered pages).

The KPI surface this driver pulls in (slab, dma-fence, scatterlist, the shmem page provider, the
synchronous workqueue, …) and the cooperative-UP model behind §3.2 are the general shim — see
[linuxkpi.md](linuxkpi.md) §2–§3.

---

## 4. Why this approach

virtio-gpu was chosen as the first LinuxKPI driver because its device interface is a clean virtqueue
protocol, not register-banging specific silicon — so the weight is on shimming the reusable
**DRM/KMS + virtio core**, which is the surface a later `i915` / `iwlwifi` would also need. Running
the driver *unmodified* (rather than rewriting it against NanOS APIs) is the whole point: it proves
the shim path and tracks upstream.

---

## 5. Real hardware: graceful no-op

`virtio-gpu` is a virtual (QEMU) device. The real Dell Latitude has Intel integrated graphics and
**no** `1AF4:1050` device. The kext loads, `knx_pci_find()` returns nothing, and `nkext_init()`
returns `-1` **before** touching `/dev/fb0`. The desktop then renders on the firmware framebuffer
(§2), unchanged. The two display paths are mutually exclusive and selected automatically by device
presence — adding the virtio_gpu kext to the image does not affect a machine that lacks the device.

---

## 6. Testing

- **Host doctests** (`make test64`) cover the shim primitives in isolation (slab, idr, scatterlist,
  sort, jiffies, identity DMA, the `%p` printf variants). The shim headers are kept host-clean with
  `#ifndef NANOS_HOST_TEST` guards so they don't clobber libstdc++/glibc in the doctest build.
- **`smoke-virtio-gpu`** (`scripts/smoke-virtio-gpu.sh`, wired into `verify64`) boots
  `-vga none -device virtio-gpu-pci` — virtio-gpu as the **only** display, so any pixels prove the
  driver drove them — and asserts the unmodified driver probes, the scanout/fb0 bridge comes up, and
  the `nwm` desktop renders (a colour-rich frame) with no kernel fault.

---

## 7. GPL boundary

The vendored Linux DRM/virtio source (GPLv2) compiles only into `virtio_gpu.nkext` and is never
linked into `kernel.bin` — see [linuxkpi.md](linuxkpi.md) §5.

---

## 8. Files (virtio-gpu-specific)

The shim and vendored source are listed in [linuxkpi.md](linuxkpi.md) §8; the display-specific glue:

| Path | What |
|---|---|
| `kext/virtio_gpu/virtio_transport.c` | modern virtio-pci transport; INTx masking |
| `kext/virtio_gpu/virtio_gpu_drv_entry.c` | boot bootstrap: register + feature-negotiate + `virtio_gpu_probe()` |
| `kext/virtio_gpu/virtio_gpu_present.c` | scanout 0 setup + `/dev/fb0` bridge + per-frame present |
| `kernel/KernelExports.cpp` | `knx_fb_set_backing` / `knx_fb_start_present` / `knx_boot_fb` |
| `scripts/smoke-virtio-gpu.sh` | the QEMU display gate (in `verify64`) |

---

## 9. GL desktop present (virgl, `nwm-gl`)

Beside the CPU path above, nwm has an OpenGL-ES **GPU-native compositor** (`user/nwm/nw_compose_gl.c`,
Plan 1 Task 10). It composites the desktop on the GPU: the wallpaper and each window's cached content
(`w->frame`) are textures (`.bgr`-swizzled for NanOS's `0x00RRGGBB` pixels); every glass window gets a
**real GPU two-pass separable Gaussian blur** of the scene beneath it, which the window shader samples
under a rounded-corner mask at the per-window body alpha. The desktop chrome (top panel, taskbar, open
dropdown, Run/Auth modals) is the one thing kept on the CPU 2D toolkit — `nw_compose_chrome` renders it
into a transparent black-keyed overlay that the GPU draws last; the cursor is a keyed quad and the modal
desktop-dim is a GPU quad. The composited frame is scanned out through the canonical Linux GPU path — a
GBM scanout surface + EGL ES context (`user/glkms/glkms_init.c`), `eglSwapBuffers` → `drmModeSetCrtc` —
so there is no CPU readback and no `/dev/fb0` blit. The host GPU (virglrenderer → ANGLE → Metal on QEMU;
i915 on the Dell) resolves the buffer to scanout.

**Fork rule (blur without hanging).** The kosmickrisp ANGLE→Metal QEMU fork deadlocks on **mid-frame FBO
attachment churn** — re-creating or re-attaching a render target inside a frame is a Metal render-pass
boundary the texture-borrow patch synchronizes on, and it hangs (same host-quirk family as its
`glReadPixels`-returns-zero). So the blur ping-pong textures **and** their FBOs are allocated once and
attached once at init (`g_blurA/g_fboA`, `g_blurB/g_fboB`, screen-sized); each window blurs into an
`fw×fh` sub-viewport corner of those fixed targets — never `glTexImage2D`/`glFramebufferTexture2D`
mid-frame. Sampling a just-rendered texture mid-frame is fine; only attachment churn was the problem.
Knobs: `NWM_NO_GLASS=1` forces opaque windows (always reachable), `NWM_GL_TRACE=1` prints a per-call
serial bracket in `blur_backdrop`; an incomplete blur FBO auto-falls back to opaque glass.

All hooks live under `#ifdef NWM_GL`, so the in-tree `nwm.nxe` is a pure-CPU program (unchanged). The
GL-capable compositor is a **separate** Mesa-linked binary: `make nwm-gl` (Docker, `build-nwm-gl.sh`
in `nanos-sdk-work/mesa-port`, the source tree mounted read-only) → `nwm-gl.nxe`, installed by
`make image64-gl` (a byte copy of `image64-grub2.img` with `/nanos/bin/nwm.nxe` swapped for
`nwm-gl.nxe`). At runtime it falls back to the CPU compositor on `NWM_NO_GL=1`, a missing DRM node
(plain QEMU), or any GL/KMS error — logging `nwm: GL compositor active` or `... unavailable ...`.

Gate: `scripts/smoke-virtio-gpu-gl.sh` (`make smoke-virtio-gpu-gl`) boots `image64-gl` on the virgl
fork QEMU, logs in on F7, and asserts `virgl 3D negotiated` + `glkms: mode WxH` + `nwm: GL compositor
active` (no `GL backend disabled`/PANIC) plus a cocoa-window distinct-colour count. It needs the fork
QEMU **and** a macOS cocoa GUI (the `gl=es`/ANGLE→Metal scanout has no headless path and the monitor
`screendump` cannot read it), so it is a developer gate — it SKIPs cleanly without the fork and is
intentionally not in headless `verify64`.

| Path | What |
|---|---|
| `user/nwm/nw_compose_gl.{c,h}` | nwm GPU-native GL ES compositor (windows + GPU glass blur + CPU chrome overlay) |
| `user/glkms/glkms_init.{c,h}` | shared GBM+EGL+KMS present sequence (also the glkms oracle) |
| `nanos-sdk-work/mesa-port/build-nwm-gl.sh` | Docker link of `nwm-gl.nxe` against the Mesa `.a` closure |
| `scripts/smoke-virtio-gpu-gl.sh` | the GL desktop gate (developer/GUI, SKIPs without the fork) |
