# LinuxKPI + virtio-gpu — Design Spec

> Status: **IMPLEMENTED** (all phases P0–P3). The **unmodified** Linux 6.12 `virtio_gpu` DRM driver
> + DRM/KMS core run on the shim and render the `nwm` desktop on `/dev/fb0` (QEMU `screendump`-verified,
> gated by `smoke-virtio-gpu` in `verify64`). Living docs: the shim is
> [`../../en/linuxkpi.md`](../../en/linuxkpi.md), the display path is
> [`../../en/graphics.md`](../../en/graphics.md).
> This file is the original approved design, kept for context.
> Roadmap stream: **H — Linux Driver Compat (LinuxKPI)** (`docs/superpowers/ROADMAP.md` §1.1).
> Date: 2026-06-28.

## 1. Goal & scope

Build a **LinuxKPI** layer — a kernel-space source-compatibility shim implementing the
leaf APIs of the Linux kernel — and use it to **recompile an unmodified Linux GPU
driver (`virtio_gpu`) and wire its scanout into NanOS's `/dev/fb0`**, so `fbcon`/`nwm`
render the desktop through a real Linux driver.

This is the **proof of the LinuxKPI path** the roadmap calls for: ≥1 real Linux driver
runs through the shim. virtio-gpu is chosen because the device interface is a clean
virtqueue protocol (not register-banging specific hardware), so the weight is on shimming
the Linux **DRM/KMS + virtio core**, which is the reusable surface for later `i915` /
`iwlwifi`.

**Success criterion (done):** NanOS boots and the `nwm` desktop renders on `/dev/fb0`
backed by the Linux `virtio_gpu` driver, verified by QEMU `screendump`.

**Internal hard checkpoint** (may stop here if needed): `virtio_gpu` probes, sets a mode,
and a test pattern is visible via `screendump` — before the `/dev/fb0` bridge exists.

### Anchored decisions

| Decision | Choice |
|---|---|
| Target driver | Linux `virtio_gpu` (DRM/KMS) on QEMU `-device virtio-gpu-pci` |
| Success | `nwm` desktop on `/dev/fb0` backed by `virtio_gpu` |
| Linux source | **6.12 LTS**, vendored snapshot of only-needed files, GPL-segregated dir |
| Architecture | **x86_64 only** (`kext64.ld`; desktop/nwm are x64; Linux drivers assume 64-bit) |
| Shim language | **C** (Linux headers are C; driver + core compile as C) |
| Shim approach | **Thin translation shim, link-error-driven** (FreeBSD LinuxKPI / `drm-kmod` model) |
| DRM core | **lifted from Linux source** (subset), not shimmed; only leaf KPIs are shimmed |
| Final artifact | one **`virtio_gpu.nkext`** (shim + lifted core + driver), loaded by existing `loadAllKexts`; `kernel.bin` untouched; module GPL-marked |

### Out of scope

3D/virgl, userspace DRM ioctls / render nodes, GBM/Mesa, multi-head, hotplug / dynamic
EDID, `i915` / `iwlwifi` (the KPI is *designed to extend* toward them but they are
follow-on after 1.0).

## 2. Why this approach (alternatives considered)

- **A — Thin translation shim, link-error-driven (chosen).** Provide a `linux/*` compat
  header tree + a `.c` implementation translating each *leaf* Linux kernel API to NanOS
  primitives (`knx_*`, kernel heap, IRQ, PCI, MMIO, DMA, threads). The DRM/virtio **core
  is compiled from vendored Linux source**, so we shim only the low-level APIs it calls,
  not DRM itself. Loop: compile → undefined symbol → implement it → repeat. This is
  exactly how FreeBSD runs Linux `drm-kmod`, and matches the roadmap's "thin shim".
- **B — Rewrite the driver against native NanOS APIs (rejected).** Destroys the entire
  point (reuse of unmodified Linux drivers), does not generalize to `i915`/`iwlwifi`,
  huge manual effort, drifts from upstream.
- **C — Full faithful Linux subsystem lift (rejected as over-engineering).** Faithful
  RCU/MM/workqueue is enormous for a PoC. We borrow one idea: lift the DRM/virtio core
  *source*, but **simplify** deep infra (cooperative workqueues, simple allocator, no real
  RCU) — which is just approach A's pragmatism.

## 3. Architecture (layers)

```
┌─ nwm / fbcon  (NanOS desktop, unchanged) ─────────────────────────┐
│                          ↓ renders to                              │
│  /dev/fb0  (Fb0Device / Framebuffer — exists)                      │
├─ BRIDGE  drm→fb0  (NEW, our code) ────────────────────────────────┤
│  drives driver's KMS: force mode = screen res, take scanout GEM    │
│  buffer, map CPU addr, on damage call driver dirty/flush           │
├─ VENDORED LINUX 6.12  (GPLv2, separate dir, compiled) ────────────┤
│  drivers/gpu/drm/virtio/      ← the actual driver                  │
│  drivers/gpu/drm/  (subset: drm_drv, KMS atomic, gem_shmem)        │
│  drivers/virtio/ + virtio_ring  ← virtio-pci modern + split ring   │
├─ LinuxKPI SHIM  (NEW, our code: linux/*.h + linuxkpi/*.c) ─────────┤
│  slab/kmalloc · device model · PCI · DMA API · ioremap/MMIO ·      │
│  threaded IRQ · workqueue · timer/jiffies · mutex/spinlock/        │
│  completion/wait_queue · printk/dev_* · scatterlist · kref/idr/    │
│  xarray · atomic/bitops · dma-fence · msleep/schedule              │
├─ NanOS PRIMITIVES  (exist: knx_* + kernel) ───────────────────────┤
│  kernel heap · IRQ table · kernel::Pci · map_mmio · dma_alloc ·    │
│  scheduler/threads · timers · /dev/fb0                             │
└────────────────────────────────────────────────────────────────────┘
```

Layer boundaries / responsibilities:

- **NanOS primitives** — unchanged kernel facilities, already exported to modules via
  `kernel/kexports.def` (`knx_malloc/free`, `knx_register_irq`, `knx_pci_*`,
  `knx_map_mmio`, `knx_dma_alloc`, `knx_uptime_us`, …). New `KX(...)` exports are added
  only where a needed facility is missing (e.g. kernel-thread spawn, waitqueue) — one
  line regenerates both resolver and import stub.
- **LinuxKPI shim** (our code) — the only layer that knows both worlds. Headers under
  `linuxkpi/include/linux/*` give the Linux API; `linuxkpi/*.c` implement it over NanOS
  primitives. Each shim unit is independently host-testable where it is pure (slab, idr,
  scatterlist, sort, jiffies math, vsnprintf).
- **Vendored Linux core + driver** (foreign GPLv2) — compiled, not modified. Sees only
  the shim's `linux/*` headers.
- **Bridge** (our code) — the only NanOS-aware consumer of the DRM driver; turns a KMS
  `drm_device` into a `/dev/fb0` backing.

## 4. KPI surface (Linux API → NanOS backing)

The exact symbol set is discovered link-error-driven; this is the expected surface.

| Category | Linux API | NanOS backing |
|---|---|---|
| slab | `kmalloc/kzalloc/kfree/krealloc`, `kmem_cache_*`, `vmalloc`, `__get_free_pages` | `knx_malloc`/`knx_free` (contiguous, identity-mapped heap); `kmem_cache` = thin wrapper; `vmalloc`→kmalloc (OK at PoC sizes) |
| device model | `struct device`, `dev_set/get_drvdata` | minimal struct + `void* drvdata`. **Bus matching bypassed** — `nkext_init` hand-builds a `virtio_device` and calls the driver `probe()` directly |
| PCI (used by virtio-pci core) | `pci_enable_device/set_master/iomap`, `pci_request_regions` | `knx_pci_find/enable_bus_master/bar`, `knx_map_mmio` |
| DMA API | `dma_alloc_coherent`, `dma_map_single/sg`, `dma_set_mask_and_coherent` | `knx_dma_alloc`; no IOMMU → `dma_addr = phys = virt` (identity); set_mask = noop-success |
| MMIO/io | `ioremap/iounmap`, `readl/writel/readw`, `ioread/iowrite` | `knx_map_mmio` + volatile deref / `IOPort` |
| IRQ | `request_irq`, `request_threaded_irq`, `free_irq` | `knx_register_irq` (start: INTx legacy via `knx_pci_irq`; MSI optional, à la e1000e); threaded = hardirq wakes a kernel thread |
| workqueue | `INIT_WORK`, `schedule_work/queue_work`, delayed work | kernel worker thread + queue (cooperative); delayed = timer + queue |
| timers/jiffies | `jiffies`, `mod_timer/del_timer`, `msleep`, `udelay`, `time_after` | jiffies from `knx_uptime_us` @ HZ=1000; timer-list serviced by a thread; sleep via scheduler |
| sync | `spinlock_t`(+`spin_lock_irqsave`), `mutex`, `completion`, `wait_event/wake_up`, `atomic_t/atomic64`, `set_bit/test_and_set_bit` | NanOS spinlocks (SMP-safe, fine-grained) + cli/sti for irqsave; blocking mutex; completion/waitqueue = waitqueue+flag; atomic/bitops = `__atomic` builtins |
| containers/ids | `kref`, `idr/ida`, `xarray` | kref = atomic refcount + release cb; small native `idr/ida`; `xarray` → lift `lib/xarray.c` only if the linker demands it |
| misc | `printk/pr_*/dev_*`, `sort/bsearch`, `scatterlist`, `WARN/BUG` | own `vsnprintf` (subset of `%p*`: `%pK/%pa/%px`) → `knx_log`; small `sort`/sg; `BUG`→halt |
| process/sched | `kthread_create/run`, `schedule/cond_resched/yield`, `current` | kernel threads (from pthread/SMP work), yield; minimal `task_struct` stub |
| dma-fence | `dma_fence_*` (virtio_gpu fences commands) | minimal shim: signal on virtqueue "used" |

## 5. Lifted Linux source (compiled, not shimmed)

- **virtio core**: `drivers/virtio/virtio.c`, `virtio_ring.c` (split ring),
  `virtio_pci_modern*` — modern virtio-pci (MMIO capabilities), not legacy.
- **DRM core (subset)**: `drm_drv`, `drm_managed` (drmm), `drm_mode_config`,
  `drm_atomic` + `drm_atomic_helper`, `drm_crtc/plane/connector/encoder`,
  `drm_probe_helper`, `drm_framebuffer` + `drm_fourcc`, `drm_gem` +
  **`drm_gem_shmem_helper`**, `drm_gem_framebuffer_helper`.
- **driver**: `drivers/gpu/drm/virtio/*`.
- **NOT lifted**: `drm_fb_helper` (fbdev emulation), debugfs/sysfs, render nodes / user
  ioctl, dynamic EDID — replaced by our bridge + one forced mode.

## 6. The `drm → /dev/fb0` bridge (our code)

After `virtio_gpu probe()` registers a `drm_device` with KMS, the bridge:

1. enumerates the connector and **forces a single mode** = NanOS's current fb resolution
   (or the preferred mode from virtio-gpu display-info),
2. allocates a GEM framebuffer and performs an **atomic commit** (plane→crtc→connector),
3. maps the GEM buffer's CPU address and **repoints `Fb0Device`** at it (today it points
   at the firmware framebuffer),
4. on NanOS damage, calls the driver flush → virtio_gpu issues `TRANSFER_TO_HOST_2D` +
   `RESOURCE_FLUSH`,
5. existing `fbcon` / `nwm` draw unchanged.

*Rejected alternative:* lift `drm_fb_helper` + shim Linux `fbdev` (`fb_info` /
`register_framebuffer`) forwarding to `Fb0Device` — more Linux reuse but a larger shim
surface. The native bridge is chosen (we already have `Fb0Device`/`Framebuffer`).

## 7. Build & GPL boundary

- **Vendored Linux 6.12** under `external/linux-6.12/` (committed, GPLv2 + `COPYING`),
  only the needed files; `scripts/vendor-linux.sh` extracts them from the official
  tarball (documents provenance).
- **Shim** under `linuxkpi/` — `linuxkpi/include/linux/*.h` (compat headers) +
  `linuxkpi/*.c` (implementation). The `-I` order puts `linuxkpi/include` first so
  `<linux/...>` resolves to the shim.
- **Flags**: `-D__KERNEL__`, a minimal generated `autoconf.h` (`CONFIG_DRM`,
  `CONFIG_VIRTIO*`, …), `-include linuxkpi/compat.h`, x86_64 kext flags
  (`-ffreestanding -fno-pic -mno-sse -mno-mmx`, appropriate `-mcmodel`), `-Wno-*` for
  upstream noise.
- **Output**: driver + core + shim + `kext_rt` → link with `kext64.ld` → `mknx` →
  **`virtio_gpu.nkext`** → the `_image` target writes it to `/nanos/kext/`. `kernel.bin`
  is untouched; the module is GPL-marked. License boundary = the KPI interface.

## 8. Testing

- **Host (doctest)** — pure shim primitives: slab + accounting, `idr/ida`, scatterlist,
  `sort/bsearch`, jiffies math, completion/waitqueue semantics (where they don't need real
  threads), identity DMA, `vsnprintf` (`%pK/%pa/%px`). Added to `TEST_MODULES` /
  `COV_PATTERNS`.
- **QEMU** — `-device virtio-gpu-pci` + `-vga none`; gates:
  - smoke: kext loads, `virtio_gpu probe → 0` (NWDBG log),
  - **P2 checkpoint**: `screendump` shows the test pattern,
  - **P3 done**: boot to `nwm` on virtio-gpu, `screendump` = desktop.
- All wired into `verify64`.

## 9. Phases (each = a plan step with a checkpoint)

- **P0 — Plumbing + loader proof.** Shim skeleton (mostly-empty `linux/*.h`), compile a
  trivial Linux-style `.c` (calls `kmalloc`/`printk`) into a `.nkext` that loads via
  `loadAllKexts`. Proves toolchain + headers + the kext path for Linux source. First host
  tests for early primitives.
- **P1 — virtio core.** Lift virtio + virtio_ring + virtio_pci_modern; shim
  PCI/MMIO/DMA/IRQ/sync; hand-build a `virtio_device` for the virtio-gpu PCI function;
  `probe()` negotiates features, sets up a virtqueue, exchanges `GET_DISPLAY_INFO`.
  **Checkpoint:** NWDBG logs display-info from QEMU.
- **P2 — DRM/GEM + modeset + test pattern.** Lift the DRM core/atomic/gem_shmem subset;
  virtio_gpu creates a `drm_device`, a resource, sets a mode; the bridge draws a test
  pattern into the scanout. **Hard checkpoint:** `screendump` = pattern.
- **P3 — `/dev/fb0` bridge.** Point `Fb0Device` at the virtio_gpu scanout, wire
  dirty/flush, boot `fbcon`/`nwm` through it. **Checkpoint (done):** desktop on virtio-gpu.

## 10. Risks & mitigations

| Risk | Mitigation |
|---|---|
| DRM lift larger than hoped (atomic/gem_shmem pull more in) | link-error-driven; stub non-essentials (debugfs/sysfs/edid); one forced mode |
| `drm_gem_shmem` expects `struct page`/shmem | the trickiest shim: a fake page-provider returning contiguous pages from `knx_dma_alloc` |
| `dma-fence` (virtio_gpu fences commands) | minimal shim signalling on virtqueue "used" |
| Linux `%p` printf variants | implement a subset in our `vsnprintf` |
| SMP safety of the shim | reuse existing fine-grained locks |
| FP/SIMD in DRM (unlikely on modeset path) | `-mno-sse`; isolate if it appears |
| GPLv2 | module kept separate, marked, never in `kernel.bin` |
| `vmalloc` non-contiguity assumption | Linux drivers rarely require it; virtio uses `dma_alloc_coherent` (we make contiguous) |

## 11. Open implementation questions (resolve during planning, not blocking)

- INTx legacy vs single-MSI for the virtio-pci IRQ (start INTx; MSI is the e1000e pattern
  if INTx is unreliable in QEMU).
- Whether `xarray` is reachable (lift `lib/xarray.c`) or `idr/ida` suffice.
- Exact new `KX(...)` exports needed (kernel-thread spawn, waitqueue) vs reusing existing.
