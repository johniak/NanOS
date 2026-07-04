# LinuxKPI — running unmodified Linux drivers on NanOS

**LinuxKPI** is a kernel-space *source-compatibility* shim: a tree of `linux/*.h` headers plus
`kpi_*.c` implementations that map the **leaf** APIs of the Linux kernel onto NanOS primitives, so an
**unmodified Linux driver** (and the kernel subsystem core it depends on) can be recompiled and run
as a NanOS `.nkext`. It is roadmap **Stream H — Linux Driver Compat**.

This document describes the **reusable** shim layer — the part that is *not* specific to any one
driver. The first consumer is the `virtio_gpu` GPU driver ([graphics.md](graphics.md)); the same
surface is the foundation a later `i915` / `iwlwifi` would build on.

---

## 1. The idea (the `drm-kmod` model)

Don't reimplement Linux drivers against NanOS APIs — that destroys the point (reuse, tracking
upstream) and doesn't generalise. Instead:

- **Compile the subsystem core from vendored Linux source**, unmodified (DRM/KMS, virtio, …).
- **Shim only the leaf APIs** that core calls (memory, PCI, MMIO, DMA, sync, ids, printk).
- Discover the exact symbol set **link-error-driven**: link all objects, list the undefined symbols,
  implement the missing leaf, repeat until it links.

This is exactly how FreeBSD runs Linux `drm-kmod`. The shim is a *pragmatic translation*, not a
faithful kernel (see §3).

```
                       <driver>.nkext
   ┌───────────────────────────────────────────────────────────┐
   │  UNMODIFIED Linux source (vendored under external/)         │
   │   the driver + the subsystem core it needs (DRM, virtio, …) │
   ├───────────────────────────────────────────────────────────┤
   │  LinuxKPI shim (linuxkpi/)  —  linux/*.h  +  kpi_*.c         │
   │   leaf APIs translated to the knx_* kext ABI                │
   ├───────────────────────────────────────────────────────────┤
   │  NanOS glue (kext/<driver>/)                                │
   │   transport / bootstrap / the NanOS-aware bridge            │
   └───────────────────────────────────────────────────────────┘
                          │ knx_* ABI
                          ▼
                     NanOS kernel
```

---

## 2. The KPI surface (Linux API → NanOS backing)

The shimmed surface, by category. (Discovered link-error-driven; this is what the DRM/virtio stack
actually pulled in.)

| Category | Linux API | NanOS backing (`linuxkpi/`) |
|---|---|---|
| slab | `kmalloc/kzalloc/kfree/krealloc`, `kmem_cache_*`, `vmalloc`, `__get_free_pages` | `kpi_slab.c` over `knx_malloc`/`knx_free` (contiguous, identity-mapped heap; size header for `ksize`/`krealloc`); `kmem_cache`/`vmalloc` = thin wrappers |
| page/shmem | `alloc_pages_exact`, `shmem_file_setup`, `shmem_read_folio_gfp` | `kpi_mm.c` + `kpi_misc.c`: each shmem mapping = one **contiguous** block sliced into page folios (`page == kernel-virtual == physical`) — the gem_shmem-style backing store |
| DMA | `dma_alloc_coherent`, `dma_map_sg/sgtable`, `dma_set_mask_and_coherent` | `kpi_dma.c`: no IOMMU → `dma_addr = phys = virt`; `dma_map_sgtable` fills `dma_address = sg_phys` |
| PCI / MMIO | `pci_enable_device/set_master`, `pci_iomap`, `readl/writel`, `ioread/iowrite` | `kpi_pci.c` + `io.h` over `knx_pci_*` / `knx_map_mmio` + volatile deref |
| scatterlist | `sg_alloc_table`, `sg_alloc_table_from_pages_segment`, `for_each_sg` | `kpi_sg.c` (coalesces contiguous identity-mapped pages) |
| sync | `spinlock_t`, `mutex`, `ww_mutex`, `completion`, `wait_event*`, `atomic_t`, `set_bit` | `spinlock.h`/`kpi_fence.c`: **cooperative UP model** — locks are no-ops, `wait_event*` spins + pumps (see §3); `atomic`/bitops = `__atomic` builtins |
| dma-fence / resv | `dma_fence_*`, `dma_resv_*`, `dma_buf_*` | `kpi_fence.c`: faithful kref + callback list + signal; cooperative wait; minimal resv/dma_buf (`drm-kmod` pattern, not lifting `dma-buf/*.c`) |
| containers/ids | `kref`, `idr/ida`, `rbtree` (augmented) | `kpi_idr.c` (flat-array idr/ida); real `lib/rbtree.c` lifted |
| workqueue | `INIT_WORK`, `schedule_work/queue_work`, delayed work | `workqueue.h`: **synchronous** — `schedule_work(w)` runs `w->func(w)` inline (see §3) |
| timers/jiffies | `jiffies`, `msleep`, `udelay`, `time_after` | `kpi_time.c`: jiffies from `knx_uptime_us` @ HZ=1000 |
| printk/misc | `printk/pr_*/dev_*`, `sort/bsearch`, `seq_file`, `sysfs_emit` | `kpi_print.c` (own `vsnprintf` incl. `%pV`/`%ps`/`%pe` → `knx_log`), `kpi_sort.c`, stubs in `kpi_misc.c` |
| device/bus | `struct device`, `dev_set/get_drvdata`, `module_*` | minimal struct + `void* drvdata`; **bus matching bypassed** — the glue hand-builds the device and calls the driver `probe()` directly |

---

## 3. Deliberate simplifications (the cooperative UP model)

The shim is sufficient for a single-producer device brought up at boot, and much smaller than a
faithful kernel. Three choices matter and differ from mainline:

- **Workqueues are synchronous.** `schedule_work()` runs the work function inline; there is no async
  worker thread. A device-completion callback runs its dequeue immediately.
- **Locks are no-ops during bring-up.** Modules load before the scheduler, single-threaded and
  non-preemptible, and a cooperative completion pump may *re-enter* the driver while a lock is held;
  a real lock would self-deadlock (and nested subsystem locks would too). No-op acquire is the
  correct UP model here.
- **No device IRQ; cooperative polling instead.** The device interrupt is masked; `wait_event*` /
  `dma_fence_wait` spin and call a pump hook that services the device, with **bounded** timeouts so a
  lost completion can't hang boot.

Real-IRQ delivery (`request_irq` over MSI/MSI-X), async workqueues + timers (worker kthreads), and a
real RCU grace period now exist (the i915 bring-up needed them):

- **RCU:** `rcu_read_lock/unlock` are no-ops, and that is *correct*, not a stopgap. NanOS uses
  deferred preemption — a task in kernel mode is never switched out except at a voluntary
  `schedule()`, and RCU readers never call one, so a reader cannot be preempted mid-critical-section.
  `synchronize_rcu()` is a **real grace period** (`Scheduler::rcuSynchronize`): it blocks until every
  other online CPU has context-switched or gone idle since the call began (either proves that CPU
  holds no pre-existing reader). UP is a barrier; the host doctest harness degrades to `smp_mb()`.
- **Shrinker:** the registry is real (alloc/link/unlink/free), but reclaim is **not wired** into
  NanOS memory pressure — a boot notice states `lkpi: shrinker registered (reclaim not wired)`.
  i915 bring-up on an 8-16 GiB machine does not depend on reclaim; wiring the OOM path is a follow-on.
- **io_mapping:** maps an MMIO aperture over `knx_map_mmio`. Write-combining needs PAT/MTRR control,
  which the kernel does not expose, so mappings are uncached (`lkpi: io_mapping UC (no PAT)`) — a
  perf follow-on, not a correctness gap.
- **request_firmware:** reads `/nanos/firmware/<name>` via the VFS; absent blobs return `-ENOENT`,
  which Gen9 i915 tolerates (no GuC/HuC/DMC on that gen).

A faithful MM (reclaim-driven shrinker) and WC io_mapping remain future work. Until a driver needs
more, the cooperative model with these upgrades is the right amount of shim.

---

## 4. Lifted vs shimmed

- **Lifted (compiled from vendored source, unmodified):** the subsystem *core* and the driver — e.g.
  the DRM/KMS core, `drm_gem_shmem_helper`, the virtio core (`virtio_ring`, `virtio_pci_modern`),
  `lib/rbtree.c`, and `drivers/gpu/drm/virtio/*`.
- **Shimmed (our `linuxkpi/`):** the leaf APIs in §2.
- **Neither (replaced by NanOS glue):** the bus-matching/device-model plumbing and the userspace-
  facing edges (fbdev emulation, debugfs/sysfs, render nodes) — the glue hand-builds the device and
  bridges the result into NanOS.

---

## 5. Build & GPL boundary

- `scripts/vendor-linux.sh` extracts the exact set of needed Linux files into `external/linux-6.12/`.
- The Makefile compiles them with `LINUXKPI_CFLAGS` + the vendored include tree, links the lifted
  objects + the shim + the glue into one `.nkext` (`mknx64`), resolving against the `knx_*` exports.
- **GPL boundary:** the vendored Linux source (GPLv2) lives only under `external/`, compiles only
  into the driver's `.nkext`, and is **never linked into `kernel.bin`** (which stays MIT-style). The
  shim (`linuxkpi/`) and the glue (`kext/<driver>/`) are the boundary.

---

## 6. Host tests

The shim primitives are unit-tested on the host (`make test64`, doctest): slab + accounting,
`idr/ida`, scatterlist, `sort/bsearch`, jiffies math, identity DMA, the `%p` printf variants. The
shim headers are kept **host-clean** with `#ifndef NANOS_HOST_TEST` guards so they don't clobber
libstdc++/glibc in the doctest build (kernel-style `min`/`max`/`static_assert`/`INT_MAX` macros, the
`pid_t`/`dev_t` typedefs, the `char*`-returning string decls, etc., are emitted only on the kext
path). Coverage is gated at ≥90%.

---

## 7. Adding another Linux driver

The surface above is reusable; a new driver is roughly:

1. `vendor-linux.sh`: add the driver + the subsystem core it needs.
2. Build it; for each undefined leaf symbol, add it to the shim (most of §2 is already there).
3. Write the NanOS glue: a transport (if the bus isn't already shimmed), a bootstrap that hand-builds
   the device and calls the driver `probe()`, and a bridge that exposes the result as a NanOS device.
4. Add host tests for any new shim primitive; add a QEMU smoke gate.

The cooperative-UP simplifications (§3) hold for a boot-time, single-producer device; a driver that
needs real concurrency would push the shim toward threaded workqueues + real IRQs first.

---

## 8. Files

| Path | What |
|---|---|
| `linuxkpi/include/linux/*.h` | the `linux/*` compat headers |
| `linuxkpi/kpi_*.c` | the leaf-API implementations (slab, idr, dma, fence, sg, print, sort, time, misc, …) |
| `external/linux-6.12/` | vendored, unmodified Linux source (GPL-segregated) |
| `scripts/vendor-linux.sh` | extracts the needed Linux files |
| `kext/<driver>/` | the per-driver NanOS glue |

The first realization is the virtio-gpu display driver — see [graphics.md](graphics.md).
