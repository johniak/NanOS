# OpenGL on the Dell Latitude 5310 (i915 via LinuxKPI + Mesa Iris) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Dell Latitude 5310 (Comet Lake-U, Intel UHD Graphics Gen9.5) runs the UNMODIFIED Linux 6.12 `i915` DRM driver through LinuxKPI — first as KMS (native-resolution display replacing the firmware framebuffer), then GEM/execbuf — and the same Mesa/GBM/EGL/nwm GL stack from the QEMU plan runs on it via the Gallium `iris` driver.

**PREREQUISITE:** `2026-07-01-plan-1-gl-desktop-virgl-qemu.md` fully implemented (DRM nodes, `mmapAt`, libdrm/Mesa ports, GBM/KMS present path, nwm GL backend, gate patterns). The Vulkan plan is NOT a prerequisite.

**Pre-implementation review (2026-07-04)** — state deltas since this plan was written, amendments applied inline below:
- Plan 1 is COMPLETE and merged to main (GPU-native compositor with GL glass blur, `9df3d96`; `drmtest`/`gles2info`/`glkms` all in `user/`). Prerequisite satisfied.
- **i915 is ALREADY VENDORED** — commit `64e49aa` brought the whole 6.12 DRM tree, including `drivers/gpu/drm/i915/**` (390 .c files) and `include/uapi/drm/i915_drm.h`. Task 5's vendoring step becomes a verification step. Note: 6.12 keeps the Intel headers in `include/drm/intel/` (e.g. `i915_pciids.h`), not `include/drm/i915_*`.
- Task 8's two nastiest latent risks are ALREADY FIXED on main: 64-bit mmap offsets (`de98534` — `I915_GEM_MMAP_OFFSET` fake offsets ≥ 4 GiB survive the syscall path) and concurrent GEM BO maps (`fe3c6e2` — per-process fb-window VA allocator + real munmap). Also `knx_dma_alloc` now allocates above `VA_USER_END` (`4747c09`) — the DMA-vs-privatized-window bug class is closed.
- **New catch (Task 2):** QEMU virtio-pci devices expose **MSI-X only** (no plain-MSI cap 0x05); `kernel/MsiRouter.cpp` finds `msixOff` but ignores it (`(void) msixOff`). Proving `request_irq` on QEMU therefore requires single-vector MSI-X support in MsiRouter. i915 on the Dell exposes classic MSI — both paths end at the same `knx_register_msi` handler contract.
- **New catch (Task 2):** the KPI spinlocks are real test-and-set locks (SMP-safe) but `spin_lock_irqsave` does NOT disable IRQs (stated in `linuxkpi/include/linux/spinlock.h`). Harmless while everything is pump-polled; a guaranteed self-deadlock once hard-IRQ handlers exist. The irqsave hardening is mandatory in Task 2, not conditional.
- **New catch (Task 2):** MSI handlers run on the CURRENT CR3 (the xHCI DMA-window lesson). Kext memory comes from the kernel heap at top-of-RAM (`KextLoader.cpp` `new[]`) so it should be visible under user CR3 — VERIFY, and assert that everything a hard handler touches (kext .text/.data, MMIO maps, vrings) is CR3-agnostic; mirror the xHCI KernelCr3 guard if not.
- **New catch (Task 3):** there is NO KPI timer wheel — `mod_timer` is a no-op stub (`linuxkpi/include/linux/timer.h`). Task 3 must implement real timers (i915 leans on delayed work: retire, hangcheck, HPD, PSR).
- **New catch (Task 6):** there is NO existing kernel cmdline/boot-param parsing to "find". The multiboot info struct has the `cmdline` field (`arch/x86_64/boot/MultibootInfo.h:20`) but nothing captures it — Task 6 builds that plumbing (capture early, expose a parse helper + knx export). Fallback if a boot path doesn't deliver a cmdline: a file knob (`/nanos/config/i915`), same pattern as the nwm knobs.
- Kext size: `virtio_gpu.nkext` is 778 KB; i915.nkext will be several MB. `loadKextImage` takes one contiguous heap allocation — Task 5's final step must confirm the alloc succeeds on the 512 MiB QEMU config.

**Architecture:** Three phases with a hard rule: **every LinuxKPI capability i915 needs that CAN be exercised on QEMU is built and gated on QEMU first** (real IRQs, kthread workqueues, firmware loader, io_mapping) using the virtio-gpu driver as the proving ground; only then is i915 vendored and brought up on the Dell, KMS-first, with the firmware-fb path as the always-available fallback. Mesa `iris` reuses the whole userspace stack — only `-Dgallium-drivers` grows. Design record: `docs/superpowers/specs/2026-07-01-gpu-stack-gl-vulkan-dell-design.md`.

**Why this is feasible at all (from the 2026-07-01 KPI gap analysis):** integrated Gen9.5 ⇒ no TTM (i915 uses its shmem GEM path for igfx — our KPI's GEM-shmem already backs virtio-gpu); GuC/HuC submission is OPTIONAL on Gen9 (execlists; `enable_guc=0` is the Gen9 default) and DMC only saves power ⇒ `request_firmware` may honestly return `-ENOENT` at first; DMA is identity-mapped and the IGP shares system RAM. The REAL new work: MSI interrupts, async workqueues, io_mapping/WC, stolen memory + OpRegion/VBT discovery, shrinker/RCU honesty, and a ~200-file compile campaign.

**Tech Stack:** LinuxKPI (extended), vendored `drivers/gpu/drm/i915` from Linux 6.12, `knx_register_msi` (exists — used by the NIC), Mesa 24.2 `-Dgallium-drivers=virgl,iris` rebuild, USB-boot Dell test protocol (conventions from the dual-boot work — documented in `docs/en/x86_64.md`, § dual-boot/Limine + real-HW notes).

## Global Constraints

- All GL-plan constraints apply (x86_64-only, unmodified vendored driver, GPL segregation into the kext, no Claude attribution in commits, CPU/firmware-fb fallback sacred, honest shims only).
- **The Dell must never be bricked into unusability:** i915 is opt-in per boot (`nanos.i915=1` boot param, default OFF on real HW until Task 9 flips it) and every failure path must leave the firmware framebuffer console working. QEMU auto-enables the KPI features (they're driven by virtio-gpu there).
- **No i915 code lands until Phase A (Tasks 2-5) is verify64-green on QEMU** — KPI upgrades are regression-gated by the existing virtio-gpu smokes plus new unit gates.
- Real-HW testing follows the established USB-stick flow (Stream F live-USB root); every Dell session ends with results recorded in `docs/superpowers/plans/2026-07-01-dell-gpu-test-log.md` (created in Task 6) — date, image commit, result, photos/notes. No "it seemed to work" entries.
- Timeline honesty: this is the largest single lift since SMP. Tasks are sized so each is independently verifiable; expect Phase B (bring-up) to iterate.

---

## File Structure

**Created:**
- `linuxkpi/kpi_irq.c` — `request_irq`/`request_threaded_irq`/`free_irq` over `knx_register_msi`.
- `linuxkpi/kpi_kthread.c` — kthreads + real (async) workqueues over kernel threads.
- `linuxkpi/kpi_firmware.c` — `request_firmware` from `/nanos/firmware/` via a knx VFS-read export.
- `linuxkpi/kpi_iomap.c` — `io_mapping_*` + `ioremap_wc` (WC via PAT if available, else UC — measured, not assumed).
- `kext/i915/i915_kext.c`, `kext/i915/i915_drv_entry.c` — probe glue (hand-built `pci_dev`, stolen mem, OpRegion).
- `kext/i915/knx_i915.h` — boot-param + fb-handoff surface.
- `scripts/smoke-kpi-irq.sh`, `scripts/smoke-kpi-wq.sh` — QEMU gates for the new KPI (via virtio-gpu/fence paths).
- `scripts/build-i915.sh` — the compile-campaign driver (object list + progress count, like the DRM-core lift used).
- `docs/superpowers/plans/2026-07-01-dell-gpu-test-log.md` — the running Dell test log.
- `docs/en/real-hw-gpu.md` — Dell GPU test protocol (USB prep, boot params, fallback, log capture).

**Modified:**
- `external/linux-6.12/` — vendor `drivers/gpu/drm/i915/**` + `include/drm/i915_*` via `scripts/vendor-linux.sh` (extend its file list).
- `linuxkpi/compat.h`, `autoconf.h`, `include/linux/*` — the i915 compile campaign's KPI growth.
- `kernel/kexports.def`, `kernel/KernelExports.*`, `linuxkpi/lkpi_knx.h` — `knx_file_read`, `knx_thread_spawn`, `knx_pci_cfg_read16`, stolen-mem/E820 export as needed.
- `Makefile` — `I915_OBJS` + `i915.nkext`, smoke wiring.
- `user/nwm` — nothing (the GL backend probes `/dev/dri` — device-agnostic by design).
- `docs/en/linuxkpi.md`, `docs/en/graphics.md` — i915 sections.

---

## Phase A — LinuxKPI upgrades, proven on QEMU (Tasks 1-5)

### Task 1: Dell hardware inventory (30 minutes on the machine, zero code risk)

Ground truth before anything: exact GPU PCI ID, BARs, OpRegion pointer, stolen-memory base/size, panel via the existing boot.

**Files:**
- Modify: whatever file prints the PCI scan at boot (find: `grep -rn "PCI" kernel/*.cpp | grep -i "scan\|probe" | head`) — extend the boot log for display-class (0x0300) devices to also print: BAR0/BAR2 base+size, config dword at 0xFC (ASLS/OpRegion), config dword at 0x5C (BDSM/stolen base), config word at 0x50 (GGC — graphics control, stolen size bits). All reads via the existing `knx_pci_cfg_read32` pathway (add a 16-bit variant if missing).
- Create: first entry in `docs/superpowers/plans/2026-07-01-dell-gpu-test-log.md`.

**Interfaces:**
- Produces: a committed log entry with the real values. Expected (verify, don't trust): vendor 8086, device **0x9B41 or 0x9BCA** (CML-U GT2), BAR0 16 MiB (GTTMMADR), BAR2 256 MiB (GMADR/aperture), nonzero ASLS, BDSM aligned to MiB, GGC stolen size ≥ 32 MiB. These constants parameterize Tasks 7-8.

- [ ] **Step 1:** Extend the boot PCI log (guarded to display-class devices; ~20 lines). Verify on QEMU first: the virtio-gpu device prints its BARs, no crash. `make verify64` green.
- [ ] **Step 2:** Build the USB image exactly as the dual-boot flow prescribes (see `docs/en/x86_64.md`, the dual-boot/Limine + Dell sections; `touch user/init.c` first so the on-screen build stamp proves image freshness — known pitfall), boot the Dell, photograph/transcribe the values into the test log.
- [ ] **Step 3: Commit** (code + log entry): `git commit -m "pci: boot-log display-adapter BARs/ASLS/BDSM/GGC + Dell 5310 GPU inventory"`.

### Task 2: Real interrupts in LinuxKPI (`request_irq` → MSI), proven on virtio-gpu

Replace the polling-only model where it matters: wire `request_irq`/`request_threaded_irq` to `knx_register_msi`, and let the virtio-gpu driver's own IRQ path (vq interrupt → `virtio_gpu_fence_event_process`) run interrupt-driven on QEMU. The cooperative pump REMAINS as fallback (devices without MSI, and the poll hook stays as a watchdog).

**Files:**
- Create: `linuxkpi/kpi_irq.c`; Modify: `linuxkpi/include/linux/interrupt.h` (real decls replacing stubs), `kext/virtio_gpu/virtio_transport.c` (register the device's MSI → call the driver's vq interrupt handler instead of relying purely on the pump), `Makefile`, `scripts/smoke-kpi-irq.sh`

**Interfaces:**
- Consumes: `int knx_register_msi(bus, dev, func, void (*h)(void*), void *ctx)` (`linuxkpi/lkpi_knx.h:34` — exists, NIC-proven). **Constraint to verify first:** what context does the kernel invoke the handler in (hard-IRQ with which stacks/locks/CR3)? Read the NIC's usage (`grep -rn "knx_register_msi" kernel/ net/ drivers/ | head`) and mirror its discipline. Handlers run on the CURRENT CR3 — audit that everything they touch is CR3-agnostic (see the 2026-07-04 review notes).
- **MSI-X sub-task (required for the QEMU proving ground):** QEMU virtio-pci exposes MSI-X only; `kernel/MsiRouter.cpp:msiSetup` already locates `msixOff` but discards it. Add single-vector MSI-X programming (map the table via BAR/BIR from the cap, write one entry, unmask) behind the SAME `knx_register_msi` contract, preferring plain MSI when both exist. The Dell's i915 has classic MSI, so this path is QEMU-only insurance — but it is what lets Task 2 be proven before the Dell.
- Produces:

```c
/* kpi_irq.c */
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
                const char *name, void *dev);
int request_threaded_irq(unsigned int irq, irq_handler_t handler,
                         irq_handler_t thread_fn, unsigned long flags,
                         const char *name, void *dev);
void free_irq(unsigned int irq, void *dev);
/* The kext maps its device's MSI to a linux "irq number" via: */
int lkpi_irq_bind_msi(unsigned bus, unsigned dev, unsigned func); /* returns the irq no. */
```

Hard-IRQ handlers run directly in the MSI callback; `thread_fn` runs via the Task-3 workqueue (until Task 3 lands, `request_threaded_irq` runs thread_fn inline right after the hard handler when it returns `IRQ_WAKE_THREAD` — documented interim, replaced in Task 3, acceptable because virtio-gpu's handler is hard-only).

- [ ] **Step 1:** Host-test the irq-table logic (bind/lookup/free, double-free guard) as pure functions (doctest).
- [ ] **Step 2:** Implement; convert `virtio_transport.c` to register the interrupt (MSI-X on QEMU, see above) and call the driver's configured vq callbacks. The current pump chain is: `lkpi_wait_pump()` → `entry_vq_poll` (installed via `lkpi_set_fence_poll`, `kext/virtio_gpu/virtio_gpu_drv_entry.c:118`) → `vt_interrupt`-style ISR harvest in `virtio_transport.c` — trigger the same harvest function from the interrupt; the pump stays as watchdog.
- [ ] **Step 3:** `scripts/smoke-kpi-irq.sh`: boot plain QEMU, assert serial `lkpi: irq N bound (msi)` + the desktop still renders + a new counter line `lkpi: irq fired>0` printed once after first interrupt. Wire into verify64.
- [ ] **Step 4:** `make verify64` — ALL existing gates green (especially smoke-virtio-gpu, smoke-vt, SMP gates: MSI handler concurrency vs fine-grained locks). **Mandatory hardening (verified 2026-07-04):** the KPI spinlocks are already real test-and-set locks, but `spin_lock_irqsave` does NOT disable local IRQs — with hard-IRQ handlers live that is a self-deadlock (handler spins on a lock held by the context it interrupted). Make irqsave/irqrestore actually save+cli / restore flags in this task.
- [ ] **Step 5: Commit** `git commit -m "linuxkpi: real MSI-backed request_irq (+hardened spinlocks); virtio-gpu goes interrupt-driven"`.

### Task 3: kthreads + async workqueues in LinuxKPI

i915 cannot run on synchronous-inline workqueues (display work, retire work, hangcheck all assume async). Provide real kernel threads.

**Files:**
- Create: `linuxkpi/kpi_kthread.c`; Modify: `linuxkpi/include/linux/workqueue.h`, `kthread.h`, `Makefile`, `scripts/smoke-kpi-wq.sh`
- Modify: `kernel/kexports.def` + `KernelExports.cpp`: `knx_thread_spawn(void (*fn)(void*), void *arg, const char *name)` — expose the scheduler's kernel-thread creation (find it: `grep -rn "kernel thread\|kthread\|spawnKernel" kernel/ | head`; the SMP scheduler has kernel-context tasks — reuse, do not invent).

**Interfaces:**
- Produces: `kthread_run/kthread_stop/kthread_should_stop`; `alloc_workqueue` returns a queue backed by ONE kthread each (i915 creates few; cap 16 with fail-loud), `queue_work/queue_delayed_work`, `flush_workqueue/flush_work/cancel_*` with real semantics (completion-based); `schedule_work` migrates from inline to the system workqueue thread. `request_threaded_irq`'s thread_fn moves onto a dedicated irq thread (fixing Task 2's interim). **Timers (verified 2026-07-04): there is NO kpi timer wheel — `mod_timer` is a no-op stub in `linuxkpi/include/linux/timer.h`.** This task builds real timer semantics: either a deadline list scanned by a dedicated timer kthread (simplest, honest) or a wheel over the kernel tick — `queue_delayed_work` AND `mod_timer/timer_setup/del_timer_sync` both ride it (i915 uses both; hangcheck/retire/HPD depend on them actually firing).
- **Regression risk is THE point:** inline→async changes ordering the virtio-gpu bring-up implicitly relied on. The smoke suite is the referee.

- [ ] **Step 1:** Host-test queue semantics (enqueue/flush/cancel state machine) with a fake thread hook.
- [ ] **Step 2:** Implement over `knx_thread_spawn`; keep a `LKPI_WQ_INLINE=1` build-time escape (default off) for bisecting regressions.
- [ ] **Step 3:** `smoke-kpi-wq.sh`: serial markers `lkpi: wq system up (thread)` + desktop renders + `smoke-virtio-gpu` internals unchanged. verify64 fully green (this WILL shake out latent assumptions — budget iteration here).
- [ ] **Step 4: Commit** `git commit -m "linuxkpi: kthreads + async workqueues (real flush/cancel semantics)"`.

### Task 4: `request_firmware` + io_mapping + honest shrinker/RCU minimums

The remaining i915 table stakes, all QEMU-buildable (runtime exercised on the Dell).

**Files:**
- Create: `linuxkpi/kpi_firmware.c`, `linuxkpi/kpi_iomap.c`; Modify: `linuxkpi/include/linux/` (firmware.h, io-mapping.h, shrinker.h, rcupdate.h), `kernel/kexports.def` (`knx_file_read(const char *path, void *buf, unsigned long max, unsigned long *out_len)` — implement over the kernel VFS the way init loads binaries; find that path first).

**Interfaces & scope (each honest, none faked):**
- `request_firmware(&fw, name, dev)`: read `/nanos/firmware/<name>`; absent → `-ENOENT` (i915 tolerates for GuC/HuC/DMC on Gen9). `release_firmware` frees. Image build: `make image64` gains an empty `/nanos/firmware/` dir + README (firmware blobs are optional, documented — if we later ship DMC, its license (redistributable linux-firmware) is noted).
- `io_mapping_create_wc/map_wc/unmap`: over `knx_map_mmio`; WC only if the kernel exposes PAT/MTRR control — check (`grep -rn "PAT\|MTRR" kernel/ | head`); if not, UC with a logged notice `lkpi: io_mapping UC (no PAT)` — correctness first, perf follow-on.
- Shrinker: `register_shrinker` keeps a real list; a `knx` hook lets the kernel's OOM path call it later, but wiring into NanOS memory pressure is a RECORDED FOLLOW-ON — for now the registry exists, is never invoked, and a boot log states that (`lkpi: shrinker registered (reclaim not wired)`). i915 must survive without reclaim on a 8-16 GiB machine for bring-up.
- RCU: with SMP real, audit the existing stubs: `synchronize_rcu` must actually wait for all CPUs to schedule (implement over the kernel's existing IPI/scheduler facilities — the TLB-shootdown machinery proves the primitive exists); `rcu_read_lock/unlock` map to preempt-disable equivalents per the deferred-preemption scheduler's rules. Get this REVIEWED against the scheduler docs (`docs/` scheduler notes) before merging.

- [ ] **Step 1:** doctests for firmware path resolution + shrinker registry.
- [ ] **Step 2:** implement; boot QEMU: markers present, verify64 green.
- [ ] **Step 3: Commit** `git commit -m "linuxkpi: request_firmware, io_mapping, shrinker registry, real synchronize_rcu"`.

### Task 5: Vendor i915 + compile campaign (link-error-driven, QEMU-buildable)

Vendor the driver and grind it to zero compile/link errors exactly like the 49-file DRM-core lift — this is mechanical but long; the script keeps score.

**Files:**
- **Vendoring is ALREADY DONE** (commit `64e49aa` brought the whole DRM tree: `drivers/gpu/drm/i915/**` = 390 .c files, `include/uapi/drm/i915_drm.h`, Intel headers under `include/drm/intel/`). Step 1 shrinks to verifying tree completeness against the 6.12 i915 Makefile. Create: `scripts/build-i915.sh`; Modify: `Makefile` (`I915_OBJS`, `i915.nkext` linking `kext/i915` glue + I915 objs + shared DRM core/lib + LINUXKPI objs), `linuxkpi/autoconf.h` (the `CONFIG_DRM_I915_*` block)

**Interfaces:**
- Produces: `bin/i915.nkext` links with zero unresolved symbols. Object list = Linux's `drivers/gpu/drm/i915/Makefile` translated with these CONFIG choices (write them as comments in `I915_OBJS`): `CONFIG_DRM_I915=y`, `DEBUG*` off, `CONFIG_DRM_I915_CAPTURE_ERROR=n` (drops error-capture deps), `CONFIG_DRM_I915_PXP=n`, `CONFIG_HWMON=n`, `CONFIG_PERF_EVENTS=n` (i915_pmu compiled out — verify the Makefile guards), `CONFIG_DRM_FBDEV_EMULATION=n` (we hand /dev/fb0 over ourselves, mirroring the virtio kext), display=y, GVT=n.
- Campaign discipline (same as the DRM lift): fix by (a) vendoring a real `include/linux/*.h`, (b) real KPI impl in `linuxkpi/`, (c) force-include, in that order; NEVER edit `external/linux-6.12/**`. Track progress in the commit messages (`N of ~210 objects clean`). Expect the big clusters: `i915_gem_*` (shmem/shrinker/mman), `intel_display/*` (DP/DDI/PSR), `intel_gt/*` (engines/execlists/breadcrumbs), `i915_irq.c`, `intel_uncore.c` (forcewake — real MMIO discipline), `intel_runtime_pm` (shim to always-awake with refcount asserts kept).

- [ ] **Step 1:** verify the vendored tree is complete vs the 6.12 i915 Makefile (already in tree via `64e49aa` — re-run `scripts/vendor-linux.sh` only if files are missing) + `scripts/build-i915.sh` printing `clean/total` objects.
- [ ] **Step 2..N:** iterate clusters; commit per cluster (the git history IS the campaign log, as with drm core).
- [ ] **Final step:** `i915.nkext` links; QEMU boots with it PRESENT but idle (no 8086 display device → probe never runs): `make verify64` green with the kext on the image. The kext will be several MB (virtio_gpu.nkext is 778 KB; i915 is ~5-10× that) and `loadKextImage` takes ONE contiguous heap allocation — confirm it succeeds on the 512 MiB QEMU config, and fail loud with the size in the message if the heap can't serve it.
- [ ] **Commit** `git commit -m "i915: full 6.12 driver compiles + links against LinuxKPI (kext idle without hw)"`.

---

## Phase B — Dell bring-up, KMS first (Tasks 6-8)

### Task 6: probe glue + Dell test protocol (display OFF — driver init up to modeset, then hand back)

**Files:**
- Create: `kext/i915/i915_kext.c` (kext entry: find PCI 8086/display-class via `knx_pci_find`, build `pci_dev` with the Task-1 BARs, wire config-space accessors), `kext/i915/i915_drv_entry.c` (replicates `i915_pci_probe`'s prologue: match against the driver's own `pciidlist` — the CML entries must match Task 1's device id; stolen memory node from BDSM/GGC; OpRegion mapping from ASLS via `knx_map_mmio`), `kext/i915/knx_i915.h`, `docs/en/real-hw-gpu.md`, test-log entries
- Modify: boot-param plumbing: `nanos.i915=1` — **there is NO existing kernel boot-param parsing (verified 2026-07-04)**. The multiboot info struct carries `cmdline` (`arch/x86_64/boot/MultibootInfo.h:20`) but nothing captures it: build the plumbing (stash the string in early boot before the multiboot pages are recycled, add a `bootParamBool("nanos.i915")` helper + knx export for kexts). Verify BOTH boot paths (GRUB2 multiboot and Limine) actually deliver a cmdline; if one doesn't, the documented fallback is a file knob `/nanos/config/i915` (kexts load from the rootfs anyway). Default 0 on real HW, forced 0 when a virtio-gpu already bound the display.

**Interfaces:**
- Produces: with `nanos.i915=1` on the Dell, serial-to-screen markers (the fbcon is still firmware-fb — markers print there): `i915: probe start`, `i915: uncore/forcewake OK`, `i915: GGTT init OK (…)`, `i915: display: NOT taking over (bringup mode)`, and a controlled stop BEFORE modeset (a `knx_i915.h` flag `i915_bringup_stop_before_modeset=1`). Every marker failure → clean abort, firmware fb intact, log entry.
- Consumes: Task-1 constants, Phase-A KPI.
- **Dell log capture:** no serial port — markers ALSO append to `/nanos/log/i915-boot.txt` on the USB stick's writable root (the live-USB rw root exists — Stream F); after a hang, the file survives reboot. Photos for panics. Document both in `real-hw-gpu.md`.

- [ ] Steps: implement glue → QEMU regression (kext idle) → Dell boot with `nanos.i915=1`, iterate marker by marker (each Dell session = one test-log entry; fix, rebuild, re-flash via the documented flow) → commit when `GGTT init OK` is reached (`git commit -m "i915: probes on the Dell up to GGTT (display untouched)"`).

### Task 7: KMS milestone — native-res display on the Dell

Let init proceed through display: VBT from OpRegion → DDI/eDP → modeset; then hand the framebuffer to NanOS exactly like the virtio kext does (`knx_fb_set_backing` + present hook → fbcon + /dev/fb0 + nwm-on-CPU at native 1920×1080).

**Files:**
- Modify: `kext/i915/i915_drv_entry.c` (drop the stop-flag path; add the fb handoff mirroring `kext/virtio_gpu/virtio_gpu_present.c`'s pattern — for i915 the scanout is a GEM BO whose pages are CPU-mappable; present hook is a no-op (panel scans out directly) but damage tracking stays for parity), `Makefile` (image installs i915.nkext by default; boot param still gates)

**Success criterion (the milestone the whole phase exists for):** Dell boots with `nanos.i915=1` → fbcon text at native res → nwlogin greeter → CPU-composited desktop at 1920×1080 (today it runs at the firmware mode). Backlight works (eDP backlight via the panel path; if the BLC needs the PCH PWM and it fails, brightness is a recorded follow-on, not a blocker — panel visible = pass).
**Failure = fallback:** any error → abort + firmware fb (the code keeps the firmware fb backing until the very last switch — atomic handoff, same discipline the virtio kext used).

- [ ] Steps: implement handoff → QEMU regression green → Dell iterate (VBT parse, DDI training, PLL — the markers narrate; `drm.debug=0xe`-equivalent: enable the vendored DRM debug logs via the KPI's printk gate for these sessions) → test-log entry with photo → flip `nanos.i915` default to 1 for the Dell image ONLY after 3 clean boots + one full desktop session → commit (`i915: native-res KMS on the Dell 5310 (eDP), firmware-fb fallback intact`).

### Task 8: GEM/execbuf — the render engine executes a batch

GPU commands run: context + VM (full ppGTT on Gen9), execlists submission (no GuC), breadcrumb interrupts (Phase-A MSI), and the canonical oracle: a batch buffer of `MI_STORE_DWORD_IMM` writing a magic value into a BO, CPU-verified (what igt's `gem_exec_store` does).

**Files:**
- Create: `user/i915test/i915test.c` — raw-ioctl (mirrors drmtest): `I915_GEM_CREATE` → `I915_GEM_MMAP_OFFSET` + `mmapAt` (shmem BO path — extend `node_mmap_offset`'s kext twin in `kext/i915` reusing the same `knx_drm_ops` shape registered for card1/renderD129... **NO** — one DRM node registrar serves whichever driver bound; the kext calls `knx_drm_register` exactly like virtio's did; only ONE GPU driver binds per machine, cleanly) → `EXECBUFFER2` with the store batch → wait (`I915_GEM_WAIT`) → assert the dword.
- Modify: `kext/i915/i915_drv_entry.c` (register DRM nodes post-KMS), `docs`.

- [ ] Steps: node registration on Dell → `i915test` markers `i915test: store OK` on the Dell (each engine-hang iteration → test-log; hangcheck works thanks to Task-3 workqueues; a hang recovers via engine reset and the machine stays alive — verify reset actually recovers by inducing one bad batch deliberately ONCE, documented) → commit (`i915: execbuf executes user batches on the Dell (gem_exec_store oracle)`).

---

## Phase C — Iris + desktop (Tasks 9-10)

### Task 9: Mesa rebuild with iris + Dell GL desktop

**Files:**
- Modify (external): `$(SDK_WORK)/mesa-port/hooks/pre_configure.sh`: `-Dgallium-drivers=virgl,iris` (iris needs no LLVM; it pulls the in-tree Intel compiler + genxml — expect new build-host deps only (python mako already there)); rebuild; `MESA_LIBS` closure grows (`libiris`, `libintel_*`, `libisl`).
- No nwm changes: `glkms_open` on the Dell finds `/dev/dri/card0` = i915, GBM/EGL/GLES2 identical.

- [ ] Steps: rebuild + relink `gles2info`/`glkms`/nwm → QEMU regression (virgl still selected there — verify64 + GL smoke green) → Dell: `gles2info` prints `renderer=Mesa Intel(R) UHD Graphics (CML GT2)`; `glkms` gradient at native res; desktop with `nwm` GL backend + GPU blur — the plan's end state → test-log entry with photos → commit.

### Task 10: Gates + docs + follow-on record

- [ ] **CI gate `smoke-i915-build`:** `scripts/build-i915.sh --assert-clean` + kext link + the kext-on-image QEMU boot (idle probe) — the compile/link/regression protection that CAN run without the Dell; wire into verify64.
- [ ] **Dell checklist** (`docs/en/real-hw-gpu.md`): the 6-item manual acceptance run (boot, native-res console, desktop CPU, desktop GL, i915test, fallback boot with `nanos.i915=0`) — this is the recurring release gate for real HW, mirroring how the dual-boot work documented its Dell flow.
- [ ] **Docs:** `linuxkpi.md` (the KPI grew: irq/kthread/wq/firmware/iomap/rcu — update the surface table), `graphics.md` (i915 path + boot params + fallback), ROADMAP tick.
- [ ] **Recorded follow-ons:** brightness/PWM if deferred; runtime PM (battery!); shrinker reclaim wiring; dock/HDMI outputs; ANV (Vulkan plan Task-6 note); i915 on 2 more CML machines before calling it stable.
- [ ] **Commit** docs + gates.

---

## Self-Review

**Feasibility honesty:** the two claims everything rests on — (a) Gen9 runs GuC-less on execlists with `-ENOENT` firmware, (b) igfx i915 uses shmem GEM (no TTM) — are stated as *verified-during-Task-5* assumptions: the compile campaign reads the 6.12 source of `intel_uc_wants_guc()` / `i915_gem_object_get_pages_shmem` and the implementer confirms both in the Task-5 cluster commits. If 6.12 moved CML to required-GuC (it did not, but VERIFY), the fallback is shipping GuC/HuC blobs via the Task-4 loader — the plan survives either answer. ✓

**QEMU-first rule:** Tasks 2-5 all gate on QEMU (virtio-gpu exercises irq/wq; compile campaign is host-side); the Dell first runs code in Task 1 (read-only PCI log) and Task 6+ (opt-in flag, firmware-fb fallback). ✓

**Executor safety rails:** every Dell task defines its serial-less log capture, its abort-to-firmware-fb behavior, and mandates test-log entries; the boot param + atomic fb handoff prevent bricked boots; deliberate-hang test is explicit and once-only. ✓

**Consistency with the other plans:** reuses `knx_drm_ops`/`knx_drm_register`, `mmapAt`, drmtest patterns, Mesa port tree, `glkms_init` and the nwm backend untouched — the "only the kernel driver and Mesa driver differ" invariant from the design record holds. ✓
