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
`make image64-gl` (a byte copy of `image64.img` with `/nanos/bin/nwm.nxe` swapped for
`nwm-gl.nxe`). At runtime it falls back to the CPU compositor on `NWM_NO_GL=1`, a missing DRM node
(plain QEMU), or any GL/KMS error — logging `nwm: GL compositor active` or `... unavailable ...`. A
GL frame can also fail *mid-session* (e.g. an unsignalled fence after a bad present); `nwm` treats
that the same as startup failure — it tears the GL context down (wedged, not graceful: a context
that just failed a frame can hold fences that a graceful `eglTerminate` would wait on forever),
clears `g_gl` and the keyed-frame flag, marks every window's frame dirty, and forces one full CPU
repaint. From that point the desktop is the classic opaque CPU compositor for the rest of the
session — keyed glass frames are meaningless without the GL slab shader to consume them.

### 9.1 Liquid-glass window material

Every glass window (and the desktop chrome frame around it) is painted by one fragment shader,
`FS_WIN` in `nw_compose_gl.c`, as a single translucent slab rather than a flat blurred rectangle:

- A **rounded-box SDF** (`sd_box`) gives the signed distance `d` to the window's edge. A ~14 px band
  (`BEVEL`) just inside that edge is the "lens ring", built from the macOS-style edge-lens recipe
  (v3, §9.1.1 below): a circular-arc slope profile, a small-angle-Snell displacement *inward* (which
  samples content further in, i.e. magnifies what's at the rim), chromatic aberration, and a caustic
  brightening. Outside the ring the body just shows the flat, GPU-blurred backdrop (frost), itself bent
  slightly so the lens-to-frost transition doesn't shear. The bevel also carries a top-left specular
  glint + Fresnel rim and a 1 px dark outer / 1 px white inner hairline pair, so the slab reads as a
  lit, curved sheet of glass rather than a paint effect.
- A **saturation boost + diagonal gradient tint** is mixed over the lensed/frosted body (v3, §9.1.1):
  higher saturation and a cool blue-white gradient when focused, flatter/paler when unfocused, or (for
  `NW_STYLE_DARK` windows) a near-black gradient at higher density — this is what gives the Terminal
  its dark-glass look. A diagonal Aero sheen (soft white glare across the top ~40%) rides on top.
- **Caption spheres** — yellow / green / red glass balls with their own mini-lens, specular dot, and
  caustic — are rendered by the same shader directly over the classic close/maximize/minimize hit
  slots (red in the outer corner, matching the pre-glass layout), so hit-testing in `nwm`'s window
  logic is untouched: the geometry the mouse tests against never changed, only what gets drawn there.
  Unfocused windows render grey balls instead of the tri-colour set.

**Two ink contracts** decide what counts as opaque "ink" over the glass slab, both evaluated in the
same shader:

1. **Frame band ink** (title bar + borders, all windows): the CPU 2D renderer clears the whole band to
   a fully transparent ARGB canvas (`nw_compose_set_glass_frame(1)` in `nw_compose.c` +
   `nw_clear_argb`) and paints only the centred Aero title glow into it with real straight-alpha
   primitives (§9.1.2 below). The shader reads that alpha directly — `ctex.a` — as the ink coverage
   and composites it straight over the finished glass; this replaced the v1/v2 luminance key
   (`smoothstep` on the brightest channel), which could not represent near-black title text. Band ink
   is real alpha now, so any ink colour (including dark) survives.
2. **Client ink** for `NW_STYLE_GLASS_CLIENT` windows: the client's own pixels are `0xAARRGGBB` — the
   top byte is a real alpha the app controls per-pixel — and the shader does `mix(glass, content,
   alpha)` inside the client rect. Legacy windows (style 0, the default from `nw_create_window`) keep
   the old behaviour: the client rect is an opaque punch straight to `content`, glass never shows
   through it. The style word travels once, at creation, as the `c` field of `NW_REQ_CREATE_WINDOW`;
   `nw_create_window_style(d, w, h, title, style)` in `libnw` exposes it (`NW_STYLE_GLASS_CLIENT = 1`,
   `NW_STYLE_DARK = 2`, bitwise-OR'able; `nw_create_window` is just style 0). The Terminal
   (`user/terminal/terminal.c`) is the showcase: it opens with `NW_STYLE_GLASS_CLIENT | NW_STYLE_DARK`,
   paints normal cells fully opaque (solid ink) and the default background colour (cell index 0) with
   only a thin `0x50` alpha veil, so the dark glass slab shows through behind unwritten terminal space.

`NW_BORDER` is 6 px (up from 2 px pre-glass) — enough for the bevel's lens ring to read as a distinct
band around the classic frame width.

#### 9.1.1 Edge-lens recipe + shadow pass (v3)

`FS_WIN`'s rim went through a v2 (stylized quadratic displacement) and a v3 rewrite to the consensus
"macOS lens" recipe (constants at the top of `FS_WIN` in `nw_compose_gl.c`):

- `x = clamp(1.0 + d/BEVEL, 0, 1)` — 0 deep inside the slab, 1 exactly at the rim.
- `s = x / sqrt(max(1 - x*x, 0.0625))` — the circular-arc slope of that profile, floor-clamped so the
  rim pixel can't blow up into rainbow noise (max slope 4.0).
- `bend = s * (1 - 1/IOR) * thick` px — a small-angle-Snell displacement along the inward SDF gradient
  (`IOR = 1.50`); `thick` (14–26 px) scales with the window's smaller dimension and with `u_focus`
  (unfocused glass is thinner/gentler). Sampling *inward* by `bend` px is what magnifies content near
  the rim (matches "content near the edge appears pulled outward").
- `ca = g * (s * CA_PX)` (`CA_PX = 2.5`) offsets the R and B taps in opposite directions from the G tap
  — the chromatic-aberration fringe.
- `glass *= 1.0 + CAUSTIC * s * 0.25` (`CAUSTIC = 0.25`) brightens the rim proportionally to slope — the
  "light concentrates at the edge" caustic.
- The lens/frost blend itself is `lens = smoothstep(0.0, 0.7, x)`, `mix(body, ring, lens)`, where `body`
  (the flat frost) also samples with a damped `bend * 0.35` so the lens-to-frost boundary doesn't shear.
- A pseudo-3D normal `N = normalize(vec3(g*s, 1))` against a fixed key light (`LIGHT`) drives the
  specular glint; `fres = pow(x, 2.5) * 0.4` is the Fresnel-style rim brightening (Sorrell).

**Slab material** (same shader, right after the lens): saturation `mix(mix(1.0, 1.65, u_focus), 1.35,
u_dark)` over the lensed colour; a three-stop diagonal gradient tint (focused/unfocused/dark each have
their own colour+alpha stops, transcribed from the mockup CSS); a diagonal Aero sheen
(`rgba(255,255,255,.34→.10→0)`, halved when unfocused, ~0.41× when dark); then the outer/inner hairlines
(`rgba(8,16,30,.55)` outer, white `.62` inner, boosted to `~.85` at the very top edge).

**Window drop shadows** are a separate program (`FS_SHADOW`), drawn as one expanded quad *before* each
glass window's `FS_WIN` quad (`nw_gl_frame`, gated on `glass`): an analytic SDF soft box padded `SH_PAD
= 36` px beyond the frame on each side and offset `SH_OFFY = 10` px down, falling off over `20–30` px
(deeper/wider when focused) to a peak alpha of `0.60` focused / `0.42` unfocused, colour `rgba(4,10,24)`
— the mockup's two-layer shadow collapsed into one soft analytic falloff. No new textures/FBOs (the
fork rule): it's a draw-only program sharing `VS_QUAD`.

#### 9.1.2 Caption glow + auto ink polarity

The v1 titlebar drew an 8-copy ±1px halo; v3 replaces it with a real Aero glow, still CPU-rendered but
now real-alpha (`draw_caption_glow` in `nw_compose.c`):

1. Rasterize the title run's glyph coverage once into a local byte buffer (`rasterize_run_coverage`,
   same glyph walk as `nw_text`/`nw_text_argb`, VGA-fallback-aware).
2. Box-blur it twice (running-sum, radius `GLOW_R = 6` then `GLOW_R/2`) — a cheap ~Gaussian.
3. Composite the blurred sheet first (gain `4` focused / `2` unfocused, alpha-capped) at a light or
   dark sheet colour, then the crisp `cov143`-remapped core on top (dimmed to `78%` alpha when
   unfocused) — both via `nw_over_pixel` (straight alpha), so the glow's soft fringe survives all the
   way into the GL shader's `ctex.a` read (§9.1 above). Titles longer than `GLOW_MAXW - 4*GLOW_R`
   (~470 px) are clipped to fit the static glow buffer.
4. **Polarity** — light sheet + dark core over a light backdrop, or dark sheet + light core over a dark
   one — comes from `nw_backdrop_wants_dark_ink(x, y, w, h, prev)`: an 8×2 sparse sample of the
   wallpaper under the title bar, gamma-space luma, threshold ~117 with a `109..125` hysteresis band so
   a window dragged across a light/dark wallpaper boundary doesn't flicker. `NW_STYLE_DARK` windows
   (Terminal) always force a light core regardless of the sampled backdrop. The per-window decision is
   cached in `w->ink_dark` (int8, `-1` = unset) and refreshed once per dirty re-render in
   `nw_render_dirty_frames`, not per frame.

#### 9.1.3 Light-glass interiors (libnwui)

`NW_STYLE_GLASS_CLIENT` extends past the frame band into the client area: `nwui_open_style(title, w, h,
NW_STYLE_GLASS_CLIENT)` (libnwui) opens a window whose toolkit paints itself in **glass mode** instead
of the classic opaque paper background — `nwui_open(title, w, h)` is just `nwui_open_style(..., 0)`.

- The window clears to a fully transparent ARGB canvas (`nw_clear_argb`, not `nw_fill_rect`), and every
  widget paints through the straight-alpha primitive family added for this (`user/libnw/nw_gfx.{c,h}`):
  `nw_clear_argb`, `nw_over_pixel/rect/round` (src-over that also accumulates destination alpha), and
  `nw_text_argb` (glyphs through the same `cov143` gamma remap as classic text, scaled by the ink
  colour's own alpha). Any masked-RGB call (`nw_fill_*`, `nw_blend_*`, `nw_draw_char_t`) always stores
  alpha 0, which would make that pixel invisible under the GL shader — so in glass mode every paint call
  in a widget's footprint goes through the ARGB path, not just the ones that need a new colour.
- The palette (`nwui_paint.c`, straight ARGB) maps the mockup's ink/scrim language onto the toolkit:
  `GCOL_INK` (`#17222f`) / `GCOL_INK_SOFT` (same at 62%) for text, `GCOL_SCRIM` (white 25%) for panel
  bodies, `GCOL_FIELD` (white 50%) for input/list/textarea wells, `GCOL_SEL`/`GCOL_SEL_RING` (white
  22%/35%) for the selection pill + its inset hairline, `GCOL_BTN_TOP/BOT/RING` for button pills. Two
  of these were tuned in the Task 10 conformance pass, not left at the literal mockup CSS numbers:
  `GCOL_FIELD` was raised from the mockup's nominal 35% because a well backed by the v3 slab's stronger
  saturation/tint still read distinctly blue at 35%; and the focus-ring alpha used by every well
  (`argb_op(COL_TF_FOC, ...)`, was 200/255) was *lowered* to 90/255 — `glass_ring` (below) composites the
  ring across the WHOLE footprint before the inset fill goes on top, so a near-opaque accent-blue ring
  under a translucent white fill compounds into a much bluer, more opaque result than either alpha
  alone suggests; the same compounding is why `GCOL_SEL`/`GCOL_SEL_RING` were lowered from the mockup's
  literal 35%/50% (their interior compounds to something closer to the intended ~35% only once the ring
  itself is lower).
- `glass_ring(s, x, y, w, h, r, ring, fill)` fakes a translucent 1px stroke (there is no straight-alpha
  `nw_stroke_round`): it over-fills the whole rect in `ring`, then over-fills the interior (inset 1px)
  in `fill`, leaving a 1px `ring` band visible at the edge. Because both fills are true alpha composites
  (not opaque overwrites), any two colours passed here compound in the overlap — see the palette note
  above before tuning either argument in isolation.
- Generic containers (row/column/box) with an app-set background (`n->has_bg`, e.g. `.colors(fg, bg)`)
  had a mapping gap: glass mode forced alpha 255 on `n->bg`, so any app-authored panel (Files' sidebar,
  a status bar) rendered fully opaque, breaking the glass slab's continuity. The fixed contract
  (`nwui_paint.c`'s default case): a **legacy** `0xRRGGBB` colour (top byte 0 — the ordinary
  pre-glass `.colors()` call) composites as a translucent scrim at alpha `0x59`, matching the general
  field/panel alpha; a colour with a **non-zero top byte** is honoured verbatim — the app made an
  explicit alpha choice. Legacy (non-glass) windows are untouched either way.
- `NWUI_ICON_KEY`-style opaque icon PNGs blend their real per-pixel alpha through `nw_over_pixel` in
  glass mode (`nw_blend_pixel` in legacy) — icon alpha is never run through the glyph `cov143` LUT.
- CPU fallback (`NWM_NO_GL=1`): the classic compositor ignores the top alpha byte, so a glass-styled
  app's canvas shows as black wherever it painted through the ARGB path (transparent → stored as
  `0x000000`); every ink pixel still carries its own opaque RGB, so text/icons stay fully legible
  against that black canvas. Accepted cosmetic limitation of the fallback, not a bug.

#### 9.1.4 Dark-glass menubar + taskbar bands

The desktop chrome's top bar and taskbar get the same dark-glass material as a `NW_STYLE_DARK` window,
via a third program, `FS_BAR` (flat frost, no bevel/lens — the bars are thin enough that a sharp sample
+ heavy tint + saturation reads as smoked glass without a real blur pass): saturate `1.35`, tint toward
`rgba(12,17,28)` at `.42` density, plus a 1px hairline at whichever edge borders the desktop (`u_topline`
selects top vs. bottom). The two band quads are drawn in `nw_gl_frame` *after* the windows/modal-dim and
*before* the CPU chrome overlay is keyed on — so windows visibly slide **under** the bars, and the
chrome's ink (bright colours in glass mode: `NW_GLASS_INK_FG/MUT/FILL` in `nw_compose.c`, chosen to
unambiguously clear `FS_KEYED`'s ~5/255-of-black discard) sits on top of them. `FS_BAR` samples a
`glCopyTexSubImage2D` snapshot of the just-composited scene at the bar's own screen rect (reusing
`g_grab`, never an FBO attachment) rather than `g_scene_tex` directly — sampling a texture that is the
current FBO's own attachment is undefined, and the scene FBO is still bound at that point in the frame.

**Debug/escape knobs**, in addition to `NWM_NO_GL`/`NWM_NO_GLASS` above:

- `NWM_GLASS_DEBUG=1..4` replaces the slab's final colour with a diagnostic: `1` = the lens profile `x`
  (red channel, 0 deep inside → 1 at the rim), `2` = the Snell `bend` magnitude normalized to its
  theoretical max (`1.33 * 26.0` px), `3` = the sharp backdrop grab unlensed, `4` = the blurred backdrop
  grab. Useful for isolating whether a visual glitch is in the SDF/lens math, the displacement, or the
  grab/blur textures themselves.

Gate: `scripts/smoke-virtio-gpu-gl.sh` (`make smoke-virtio-gpu-gl`) boots `image64-gl` on the virgl
fork QEMU, logs in on F7, and asserts `virgl 3D negotiated` + `glkms: mode WxH` + `nwm: GL compositor
active` (no `GL backend disabled`/PANIC) plus a cocoa-window distinct-colour count. It needs the fork
QEMU **and** a macOS cocoa GUI (the `gl=es`/ANGLE→Metal scanout has no headless path and the monitor
`screendump` cannot read it), so it is a developer gate — it SKIPs cleanly without the fork and is
intentionally not in headless `verify64`.

| Path | What |
|---|---|
| `user/nwm/nw_compose_gl.{c,h}` | nwm GPU-native GL ES compositor (windows + GPU glass blur + CPU chrome overlay); `FS_WIN`/`FS_SHADOW`/`FS_BAR` |
| `user/nwm/nw_compose.c` | CPU chrome (panel/taskbar), the Aero caption glow + ink-polarity sampler |
| `user/libnw/nw_gfx.{c,h}`, `user/libnw/nw_over_core.h` | `nw_cov143` gamma LUT + the straight-alpha ARGB primitive family |
| `user/libnwui/nwui.{c,h}`, `user/libnwui/nwui_paint.c` | `nwui_open_style` + the glass-mode widget painters/palette |
| `user/glkms/glkms_init.{c,h}` | shared GBM+EGL+KMS present sequence (also the glkms oracle) |
| `nanos-sdk-work/mesa-port/build-nwm-gl.sh` | Docker link of `nwm-gl.nxe` against the Mesa `.a` closure |
| `scripts/smoke-virtio-gpu-gl.sh` | the GL desktop gate (developer/GUI, SKIPs without the fork) |
