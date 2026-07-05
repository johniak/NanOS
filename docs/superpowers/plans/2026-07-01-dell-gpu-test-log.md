# Dell Latitude 5310 — i915 GPU bring-up test log

Running log for Plan 3 (`2026-07-01-plan-3-gl-on-dell-i915-iris.md`). One entry per real-hardware
session: date, image commit, what was tested, the result (values transcribed or photographed — no
"seemed to work" entries), and the fallback state (firmware framebuffer must always survive).

The Dell has **no serial port**, so on-machine markers are captured two ways: (1) photographed from
the fbcon, and (2) appended to `/nanos/log/i915-boot.txt` on the writable live-USB root (survives a
hang + reboot). Both are documented in `docs/en/real-hw-gpu.md` (created in Task 6).

---

## Task 1 — display-adapter inventory

### Boot PCI log extension (QEMU-verified)

- **Date:** 2026-07-04 (CEST)
- **Change:** `kernel/Kernel.cpp` `pciScanReport()` now prints every PCI display-class device
  (class `0x03`): location, `vendor:device`, class/subclass, BAR0/BAR2 base+size. For Intel IGPs
  (vendor `0x8086`) it additionally dumps the graphics config registers the i915 kext needs:
  **GGC** (`0x50`, stolen size / pre-alloc bits), **BDSM** (`0x5C`, data-stolen-memory base),
  **ASLS** (`0xFC`, OpRegion pointer). Also fixed a latent `Console::writeHex(int)` bug (single-digit
  values leaked stale bytes from the shared `itoa` static buffer — `03` printed as `0350`).
- **QEMU result** (`-vga none -device virtio-gpu-pci`, `-m 512`, serial capture):

  ```
    PCI: 6 device(s); e1000 8086:100E @ 0:2.0 BAR0=... irq=10
    DISPLAY 1af4:1050 @ 0:3.0 class=03/80 BAR0=0x0(sz=0x0) BAR2=0x0(sz=0x0)
  ```

  The virtio-gpu device is display-class and is reported; no Intel IGP line (vendor is `0x1af4`).
  virtio-gpu uses modern-virtio BAR4 (not BAR0/2), hence the zeros — expected. No crash;
  `smoke-virtio-gpu` still PASSES (desktop renders, 5074 distinct colours).

### Dell hardware inventory (PENDING — needs the physical machine)

> Boot the Dell from the USB image (dual-boot/Limine flow, `docs/en/x86_64.md`; `touch user/init.c`
> before building so the on-screen stamp proves image freshness) and transcribe the `DISPLAY` +
> `Intel IGP` lines here. Expected (verify, do not trust):

| Field | Expected | Actual (Dell) |
|-------|----------|---------------|
| vendor:device | `8086:9B41` or `8086:9BCA` (CML-U GT2, Gen9.5) | _TBD_ |
| BAR0 (GTTMMADR) | ~16 MiB | _TBD_ |
| BAR2 (GMADR aperture) | ~256 MiB | _TBD_ |
| GGC (`0x50`) | stolen size ≥ 32 MiB | _TBD_ |
| BDSM (`0x5C`) | MiB-aligned base, nonzero | _TBD_ |
| ASLS (`0xFC`) | nonzero (OpRegion) | _TBD_ |

These constants parameterize Tasks 7–8 (KMS + execbuf). Fallback state after this session: firmware
framebuffer console (read-only PCI log — nothing binds the GPU).

## Tasks 2–4 — LinuxKPI upgrades (QEMU-proven, runtime lands on the Dell)

Phase A brings the KPI surface i915 needs. All are built + host-tested + QEMU-gated; the i915
*runtime* paths are exercised only on the Dell (Phase B), so QEMU proves compile/link/boot, not the
i915 code path itself.

- **Task 2 — real IRQs.** `request_irq` over MSI/MSI-X (`kernel/MsiRouter.cpp` single-vector MSI-X;
  `linuxkpi/kpi_irq.c`). QEMU: virtio-gpu delivers a real MSI to the shim handler (`smoke-kpi-irq`).
- **Task 3 — kthreads + async workqueues + timers** (`linuxkpi/kpi_kthread.c`). QEMU: inline→async
  work, real worker kthreads, desktop intact (`smoke-kpi-wq`).
- **Task 4 — firmware / io_mapping / shrinker / RCU.**
  - `request_firmware` reads `/nanos/firmware/<name>` (new export `knx_file_read`); `/nanos/firmware`
    + README staged into the image. Missing blob → `-ENOENT` (Gen9 needs none). Host doctests.
  - `io_mapping` over `knx_map_mmio`, **UC** fallback (no PAT/MTRR in the kernel) with a boot notice.
    *Dell caveat:* `knx_map_mmio` is a 32-bit-phys ABI — if the real GMADR aperture BAR sits above
    4 GiB the base truncates; widening the knx MMIO ABI is a Phase-B item.
  - Shrinker registry is real but **reclaim is not wired** (`lkpi: shrinker registered (reclaim not
    wired)`). Host doctests for alloc/register/free.
  - `synchronize_rcu` is a **real grace period** (`Scheduler::rcuSynchronize`, per-CPU switch
    counters): waits for every other online CPU to context-switch or go idle. Justified against the
    deferred-preemption model (readers can't be preempted mid-section). UP = barrier. Host doctests
    for the pure predicate (`Scheduler::rcuGraceDone`).
  - QEMU: `virtio_gpu.nkext` links the new objects and loads with all imports resolved
    (`smoke-virtio-gpu` green — `loadKextImage` fails loud on any unresolved import).

  **Dell-runtime-pending for Task 4:** actual firmware blobs (if any DMC is shipped), WC vs UC on the
  aperture, whether the GMADR BAR needs the 64-bit MMIO ABI, and RCU under real multi-CPU i915 load.

## Task 5 — compile + link (QEMU-verified) & the 64-bit BAR ABI

- The 276-object i915 compile campaign reached 100% and `bin/i915.nkext` links 0-unresolved (commit
  `e2eedc6`); isolated idle-load on QEMU boots with the driver registered, no fault.
- **64-bit MMIO/BAR ABI widened** (commit `3b795b8`): `knx_map_mmio`/`knx_pci_bar` are now 64-bit, so
  the Dell's Comet Lake GTTMMADR/GMADR BARs above 4 GiB no longer truncate — the Task-4 caveat is
  closed. Validated by `test64` 863/863 (decodes a 32-bit AND a 64-bit BAR) + runtime unregressed.

## Real-HW boot #1 — armed harness reaches the Phase-B boundary

- **Date:** 2026-07-05 (CEST)
- **Image commit:** branch `feat/i915-dell-gpu` (armed via `/nanos/config/i915` = `1`, flashed to the
  Kingston DataTraveler 3.0 with `make flash-dell-armed`).
- **Result (persisted `/nanos/logs/i915-boot.txt`, read back with `make i915-log`):** the full armed
  sequence succeeded on the real Dell — system reached shell + desktop (harness idles safe, no fault):

  ```
  i915: ===== bring-up session armed =====
  i915: mem_map init OK
  i915: DRM core init OK
  i915: unmodified Linux 6.12 i915 driver registered
  i915: Intel GPU FOUND — pci_dev construction + probe() is Phase B (Dell)
  ```

  So the LinuxKPI shim + unmodified i915 load, init, and **detect the GPU** (PCI id_table matched the
  real `8086:9B41`) on the hardware — everything short of `driver->probe()`. Fallback state: firmware
  framebuffer (harness idled without touching the display).
- **Dell inventory row (Task 1 table) still to transcribe:** the harness now logs BAR0/BAR2/IRQ at
  probe entry (see Task 6), so boot #2 will fill the GTTMMADR/GMADR/IRQ values.

## Task 6 — probe glue (build the pci_dev + drive i915's own probe)

- **Change (QEMU-built, Dell-runtime-pending):** `kext/i915/i915_entry.c` now, on a real match, hand-
  builds a `struct pci_dev` (bus/dev/func + `lkpi_pci_fill_ids` for vendor/device/subsystem/revision/
  IRQ + the six BAR `resource[]` windows decoded from live config space + a 64-bit DMA mask + a
  `pci_bus`), then calls the driver's own `probe(pdev, id)` with the matched `pci_device_id` (whose
  `driver_data` carries the CML `intel_device_info`). Params set first: `enable_guc = 0` (Gen9.5 runs
  GuC-less on execlists).
- **Stop before modeset, no source edit:** `i915.modeset=0` is the wrong knob (it makes `i915_init()`
  itself return `-ENODEV`). Instead the glue uses i915's **own** `inject_probe_failure` — a
  driver-native "abort cleanly at internal injection point N" mechanism — dialed at runtime via the
  `/nanos/config/i915_inject` knob (`make i915-inject N=<stage>` / `make i915-inject-off`). `0` = full
  probe; a crash can be walked back to the last clean stage with no rebuild/reflash.
- **Diagnostics:** the harness now tees **every** `printk` line (the full `drm_dbg` trail, once
  `__drm_debug=0x1ff`) into the persistent log too (`lkpi_set_log_tee`), so a probe that scrolls the
  fbcon or hard-hangs still leaves the complete narration on the stick. Probe entry logs BAR0/BAR2/IRQ
  and the probe return code.
- **QEMU gates green** (probe code is dead on QEMU — no Intel GPU, scan returns idle): `i915-probe`
  276/276, `link-support-probe` 21/21, `test64` doctests pass, `smoke-virtio-gpu` 3704 colours,
  `smoke-i915` A+B, `smoke-kpi-wq`/`smoke-kpi-irq`. `bin/i915.nkext` relinks 0-unresolved.
- **Next Dell boot:** armed full probe → read `make i915-log` → the `drm_dbg` trail shows how far
  probe got (uncore/forcewake → GGTT → display → …); dial `i915-inject` down to the last clean stage
  as needed. Firmware fb is the fallback throughout (nothing hands off the display until Task 7).
