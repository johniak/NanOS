---
name: nanos-linuxkpi-virtio-gpu
description: LinuxKPI shim runs UNMODIFIED Linux virtio core to drive virtio-gpu; full nwm desktop renders on it (branch feat/linuxkpi-virtio-gpu)
metadata: 
  node_type: memory
  type: project
  originSessionId: d0d0e24a-1b28-41d0-93d9-bc7079f1fd27
---

LinuxKPI (Stream H of the roadmap) — first realization. Branch `feat/linuxkpi-virtio-gpu`
(off main, not pushed). Spec `docs/superpowers/specs/2026-06-28-linuxkpi-virtio-gpu-design.md`,
plan `docs/superpowers/plans/2026-06-28-linuxkpi-virtio-gpu.md`. x86_64-only.

**What works (all QEMU-verified, screendump `-device virtio-gpu-pci`):**
- **LinuxKPI shim** `linuxkpi/` — ~40 `linux/*.h` compat headers + `kpi_*.c` over the `knx_*`
  kext ABI. Host-tested: `make test64` 829 cases green, 90.2% cov (shim primitives wired in).
- **Lifted UNMODIFIED Linux 6.12 virtio core** (`external/linux-6.12/`, GPLv2, vendored by
  `scripts/vendor-linux.sh`): `virtio_ring.c` + `virtio_pci_modern_dev.c` compile against the
  shim and LINK into one `virtio_gpu.nkext` (loaded by existing `loadAllKexts`).
- **Hand-built modern virtio-pci transport** `kext/virtio_gpu/virtio_transport.c` (virtio_config_ops
  over `vp_modern_*` + `vring_create_virtqueue`) — replaces Linux's virtio_pci_common.c bus glue
  (no SR-IOV/legacy). Probe → GET_DISPLAY_INFO returns 1280x800.
- **Display via virtio-gpu PROTOCOL** (vendored uapi structs): RESOURCE_CREATE_2D / ATTACH_BACKING /
  SET_SCANOUT / TRANSFER_TO_HOST_2D / RESOURCE_FLUSH → SMPTE test pattern, then the **full nwm
  desktop** (Files/Settings/Terminal, logged in as jan).
- **fb0 bridge**: new exports `knx_fb_set_backing` / `knx_fb_start_present` / `knx_boot_fb` +
  a kernel present thread (id 5). **Mirror mode** copies the kernel/boot framebuffer onto the
  virtio-gpu scanout each frame (reuses the proven VT/nwm graphics stack).

**Key gotchas (carry forward):**
- DMA is correct because **NanOS x86_64 RAM is identity-mapped (virt==phys)** — `dma_alloc_coherent`/
  `alloc_pages_exact` = page-aligned `knx_malloc`, `dma_handle = virt`.
- `pci_iomap_range` maxlen = bytes-from-offset, NOT total (a real bug that cost the probe).
- GCC14 makes implicit-decl / incompatible-pointer / int-conversion ERRORS; forward-declare
  `struct cpumask` at file scope (else prototype-scope mismatch).
- `pr_debug` MUST be a no-op (lifted virtio_ring.c spams the console otherwise).
- `linux/errno.h` must `#include_next` under `NANOS_HOST_TEST` — glibc's `<errno.h>` pulls
  `<linux/errno.h>` and our shim would shadow the full errno set (breaks the host suite).
- **present cadence = 30fps** (`sleepUntil(t+33)`); tighter starves userspace on 1 core and the
  login/greeter never completes.
- `loadAllKexts` runs BEFORE `Scheduler::init()` — defer thread creation (kext stores the flush
  cb; Kernel.cpp spawns the present thread after Scheduler::init via `fbStartPresentThread()`).

**Two delivered paths:**
1. **Protocol path (DONE, screendump-verified, commit `407c325`):** display via virtio-gpu device
   protocol on the lifted virtio CORE + fb0 mirror-mode. The working desktop.
2. **FULL DRM lift (compile phase DONE):** per the user's "grind pełny lift DRM" directive, the
   ENTIRE unmodified Linux 6.12 DRM compiles against the shim — **49/49 display-core DRM files +
   14/14 virtio_gpu DRM driver files = 0 errors (from 1417).** Real `include/linux/hdmi.h` vendored.
   ~120 shim headers now. Method: compile-error-driven + per-symbol-frequency batches + force-include
   leaf headers in `compat.h` (capability/uidgid/set_memory/dma-mapping/fwnode/pagemap/io/ioport/
   jump_label/file/pid/sizes/uaccess/kobject/cpufeature/fpu). gem_shmem page provider = folio==page
   over `alloc_pages_exact`; drm_mm augmented rbtree = plain-tree + linear-scan; sysfs/pseudo-fs/
   syncobj/edid stubbed (userspace-facing).
   - **CAUTION — recurring regressions:** a symbol defined in TWO force-included headers → "CLEAN
     drops to 0/48" universal error (kmemdup, div64_u64, struct resource, mapping_set_gfp_mask, fwnode
     dup, fd dup). Each force-include addition MUST check for an existing definition. Use the SAME
     `#ifndef` guard in `compat.h` as the header so the header copy is skipped.
   - **LINK phase DONE (2026-06-29):** the unmodified virtio_gpu DRM driver (14 files) + DRM/KMS core
     (59 files) + virtio core (virtio_ring + virtio_pci_modern_dev) + lib (rbtree, list_sort) + the
     LinuxKPI shim now **link into a real loadable `bin/virtio_gpu.nkext` (772 KB), 0 undefined**
     (excl. kernel `knx_*` + kext_rt memcpy/memset + libgcc). The 693→0 drop: the old 693 was a
     PARTIAL ld -r; linking ALL compiled objects together already gave 95, then resolved by:
     (a) lifting 5 more DRM core files (bridge/writeback/self_refresh/vblank_work/panel_quirks) +
     lib rbtree.c/list_sort.c — needed shims of_graph.h/dmi.h/log2.h/kmemleak.h + real
     rbtree_augmented.h (replaced the degraded one → drm_mm/vma now use correct augmented trees);
     (b) `kpi_sg.c` (sg_alloc_table family over identity-mapped pages; FIXED dma_map_sgtable which
     was a no-op leaving dma_address=0 → device DMA from phys 0); (c) `kpi_fence.c` (faithful
     dma_fence refcount+callbacks+signal+cooperative wait, minimal dma_resv/dma_buf/fence_chain —
     drm-kmod-style shim, NOT lifting dma-buf/*.c, so the 57 DRM objects keep one struct layout);
     (d) `kpi_misc.c` (shmem page provider = per-mapping lazy page-cache over alloc_pages_exact =
     gem_shmem backing, the spec's "hardest piece"; + seq_file/sysfs_emit/anon_inode/simple_strtol/
     kasprintf/idr_for_each/rb_*_cached/hdmi infoframe stubs/globals boot_cpu_data,iomem_resource,
     system_*wq,current,reservation_ww_class); (e) virtio registration glue in the new
     `kext/virtio_gpu/virtio_gpu_drv_entry.c` (__register_virtio_driver/is_virtio_device/
     virtio_reset_device — stands in for drivers/virtio/virtio.c) which calls the driver's own
     `lkpi_module_init` (shim module_driver macro made it a GLOBAL, so the driver stays UNMODIFIED)
     then vt_create() + `virtio_gpu_driver.probe(vdev)`. Makefile builds it (DRM_CORE_OBJS/
     DRM_DRIVER_OBJS/DRM_LIB_OBJS + new compile rules for drivers/gpu/drm{,/virtio} + lib).
   - **RUNTIME: boots + runs the real driver DEEP, then hangs (2026-06-29).** The new entry
     `kext/virtio_gpu/virtio_gpu_drv_entry.c` (replaces virtio_gpu_kext.c in the build): calls
     `__lkpi_modinit_drm_core_init()` (DRM core init — needed the shim module_init/module_driver
     macros to emit GLOBAL `lkpi_module_init`/`__lkpi_modinit_*` so the UNMODIFIED driver registers),
     then replicates virtio_dev_probe's FEATURE NEGOTIATION (sets vdev->features incl. transport bits
     VIRTIO_F_VERSION_1 — without this virtio_gpu_init returns -ENODEV), finalize_features, then
     `virtio_gpu_driver.probe(vdev)`. QEMU `-device virtio-gpu-pci`, serial-captured, ZERO faults:
     the real driver prints `[drm] pci: virtio-gpu-pci detected`, `features: -virgl +edid ...`,
     **`number of scanouts: 1`**, `cap sets: 0` — i.e. DRM core + GEM/fence + the real virtgpu_vq.c
     command/config path all execute on the shim. Fixes found en route: printk needed `%pV`
     (recursive va_format — the actual DRM error text) + `%ps`/`%pe`; dma_map_sgtable was a no-op
     leaving dma_address=0; shim spin/mutex made NO-OPS (UP cooperative: the vq pump re-enters the
     driver under a held lock, and DRM nested modeset locks would self-deadlock); wait_event_timeout
     made bounded+pumping (`lkpi_wait_pump`→vt_interrupt).
   - **HANG FIXED (2026-06-29) — virtio INTx interrupt storm.** Root cause was NOT the generation
     loop (that hypothesis from the unreliable disasm was wrong). Bisected with knx_log markers
     (DIAGMS/DIAGKMS/DIAGNOT through virtio_gpu_init): probe ran cleanly through modeset_init +
     device_ready + get_edids + get_display_info, and hung INSIDE `virtqueue_notify()` — the serial
     showed exactly the partial char "D" (start of the post-notify log) then froze. The MMIO notify
     write kicks QEMU, which asserts the virtio **INTx (level-triggered)** pin on used-buffer
     completion; nothing acks the PCI interrupt (we run cooperative, no device IRQ wired), so the line
     stays high and the CPU storms the unhandled vector forever — and loadAllKexts is pre-scheduler
     (non-preemptible) so it wedges all boot. (This IS the repeated ISR-register read the live disasm
     caught.) **Fix:** in `vt_create()` (virtio_transport.c), after pci_enable_device, set
     PCI_COMMAND.INTX_DISABLE (bit 10 at cfg 0x04) via knx_pci_cfg_read32/write32 so the pin never
     asserts; used-buffer completions are still harvested by the cooperative pump (lkpi_wait_pump ->
     entry_vq_poll -> vt_interrupt reads the ISR status reg -> vring_interrupt -> callback). After the
     fix the UNMODIFIED driver PROBES FULLY: EDID response 0x1104 ("QEMU Monitor", 8bpc), display-info
     0x1101 (output 0 1280x800), connector Virtual-1, `[drm] Initialized virtio_gpu 0.1.0 ... minor 1`,
     "virtio_gpu_probe() OK", and boot continues to `nanos login:` with ZERO faults. Verified QEMU
     `-device virtio-gpu-pci -serial file:` (the DIAG markers are temporary in the lifted .c files
     — REMOVE before finalizing; the pump heartbeat in kpi_fence.c + bounded status spins in
     virtio_transport.c are also debug-only and should be trimmed/kept-minimal).
   - **FULL DESKTOP RENDERS through the UNMODIFIED driver (2026-06-29) — DONE.** New glue file
     `kext/virtio_gpu/virtio_gpu_present.c` defines `virtio_gpu_fbcon_bringup(vdev)` (strong, overrides
     the weak stub): gets vgdev from `((drm_device*)vdev->priv)->dev_private`, then drives scanout 0
     ENTIRELY through the driver's OWN command layer (virtgpu_vq.c): `virtio_gpu_object_create()`
     (real shmem GEM bo → RESOURCE_CREATE_2D + RESOURCE_ATTACH_BACKING), `virtio_gpu_cmd_set_scanout(0,
     bo->hw_res_handle, w, h)`, then `knx_fb_set_backing(page_address(bo->base.pages[0]), w*4, w, h, 32,
     present_flush)` to expose /dev/fb0 over the bo backing. The periodic `present_flush()` issues the
     driver's `virtio_gpu_cmd_transfer_to_host_2d` + `virtio_gpu_cmd_resource_flush` + notify + pump
     each frame. NO hand-rolled protocol — all device I/O is unmodified driver code.
     - **Key enabler:** made the shmem provider (kpi_misc.c) allocate each mapping as ONE
       physically-contiguous alloc_pages_exact block (sliced into page folios) so
       `page_address(pages[0])` is a flat CPU framebuffer the device and CPU share (the shim has no
       vmap that stitches scattered pages; RAM is identity-mapped so contiguous-virt==contiguous-phys).
     - **VERIFIED (QEMU `-vga none -device virtio-gpu-pci`, screendump):** virtio-gpu is the ONLY
       display; tty1 fbcon (boot banner + `nanos login:`) renders, Ctrl+Alt+F7 switches to the tty7
       graphical greeter (live frame update proves the present loop), and after jan/jan login the
       FULL nwm desktop renders (wallpaper + menubar/clock + Files + Settings + Terminal jan@nanos +
       taskbar; ~3700 distinct colours). End-to-end: unmodified DRM driver → /dev/fb0 → nwm.
     - Build: `VIRTIO_GPU_OBJS += virtio_gpu_present.o`; the kext/virtio_gpu/%.c rule now uses
       DRM_VINC + -I.../drm/virtio so the glue can `#include "virtgpu_drv.h"`. virtio_gpu_kext.c (the
       old protocol path) is NO LONGER in the build — the entry is virtio_gpu_drv_entry.c. Debug DIAG
       markers reverted (driver files pristine via git checkout); pump heartbeat removed; the bounded
       status-poll guards in virtio_transport.c kept as defensive hardening.
     - **Status: the full-DRM-lift goal is realized at runtime.** Remaining polish (optional): real
       atomic-modeset/fbdev path instead of the direct command-layer scanout; cursor plane; multi-head.
   - **HOST TESTS RESTORED + QEMU GATE ADDED (2026-06-29) — spec testing phase complete.** test64
     had broad PRE-EXISTING breakage (committed by earlier DRM-lift header work, NOT this iteration —
     confirmed by `git stash` + test64 on clean HEAD failing identically): the rich shim headers
     polluted libstdc++/glibc on the host doctest path. Fixed by `#ifndef NANOS_HOST_TEST` guards so
     the kext path is byte-identical (macros kept) while host defers to libstdc++/glibc: kernel.h
     (min/max/clamp/swap/abs macros + INT_MAX/UINT_MAX/SIZE_MAX cast-macros that broke glibc's
     `#if INT_MAX==32767`), build_bug.h (`static_assert`→`_Static_assert` clobbered the C++ keyword),
     types.h (pid_t/dev_t POSIX typedefs), printk.h (`_Bool`→`bool`), string.h (memchr/strchr/strrchr/
     strstr const-overload conflicts), slab.h (+#include <linux/string.h> for memcpy). Result:
     **test64 829/829 green, 90.2% line coverage** (≥90% gate). NEW gate `scripts/smoke-virtio-gpu.sh`
     (+ Makefile `smoke-virtio-gpu` target, wired into `verify64`): boots `-vga none -device
     virtio-gpu-pci` (virtio-gpu = ONLY display, no VBE fallback), asserts serial shows
     "virtio_gpu_probe() OK" + "scanout 0 up via the unmodified DRM driver", logs in jan/jan on tty7,
     and requires a colour-rich desktop frame (3704 distinct) with no fault — PASSES.
   - **ALL SPEC PHASES COMPLETE (P0 loader, P1 virtio+display-info, P2 DRM/GEM/modeset, P3 fb0/nwm)
     + success criterion (nwm desktop on /dev/fb0 via the UNMODIFIED Linux virtio_gpu, screendump-
     verified) + testing (host doctests green + verify64 gate).** Final artifact = one virtio_gpu.nkext
     (shim+lifted core+unmodified driver), kernel.bin untouched, GPL-segregated. Changes uncommitted
     (committed protocol-path desktop 407c325 unaffected; commit only when the user asks).
     [[nanos-nic-e1000e-i219]]
