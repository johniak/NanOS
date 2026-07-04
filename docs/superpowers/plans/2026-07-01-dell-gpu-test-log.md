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
