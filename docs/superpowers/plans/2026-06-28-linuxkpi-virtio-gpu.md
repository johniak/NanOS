# LinuxKPI + virtio-gpu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a kernel-space LinuxKPI source-compat shim and use it to recompile the unmodified Linux `virtio_gpu` driver, wiring its scanout into NanOS `/dev/fb0` so `nwm` renders on a real Linux GPU driver.

**Architecture:** Thin translation shim (FreeBSD `drm-kmod` model). The Linux DRM/virtio **core is compiled from vendored Linux 6.12 source**; only the *leaf* kernel APIs it calls are shimmed (link-error-driven). The driver + core + shim link into one `virtio_gpu.nkext` (GPLv2, loaded by existing `loadAllKexts`); `kernel.bin` is untouched. A native `drm→fb0` bridge (our code) drives the DRM driver's KMS and repoints `Fb0Device` at the scanout.

**Tech Stack:** C (shim + Linux source), C++ (bridge, matching kext convention), NanOS kext build (`KEXT_CFLAGS`, `kext64.ld`, `mknx64`), doctest host tests, QEMU `-device virtio-gpu-pci` for integration.

## Global Constraints

- **Arch: x86_64 only** — `ARCH=x86_64`, `kext64.ld` (base `0x800000`), `mknx64` (v4 / R_X86_64_*). i686 path not built for this module.
- **Ring-0 codegen** — `KARCHFLAGS` = `-mno-red-zone -mno-sse -mno-mmx -mno-80387`; plus `-ffreestanding -fno-pic -fno-stack-protector`. No SSE/x87/red-zone.
- **Linux source: 6.12 LTS**, vendored under `external/linux-6.12/` (committed, GPLv2 + `COPYING`), only needed files, extracted by `scripts/vendor-linux.sh`.
- **GPL boundary** — vendored Linux + shim link into `virtio_gpu.nkext` only; never into `kernel.bin`. Module is GPL-marked. Boundary = the KPI interface.
- **Shim include precedence** — `-I linuxkpi/include` first so `<linux/...>` resolves to the shim, not host/freestanding headers.
- **No new kernel-image deps** — new kernel facilities exposed only via `KX(...)` lines in `kernel/kexports.def` (regenerates resolver + import stub).
- **Loud failures** — an unresolved import must fail the kext load loudly (existing `NxeLoader` behaviour); never silently stub a symbol the driver depends on for correctness.
- **No Claude attribution** in commits.

---

## File Structure

**New — shim (our code, C):**
- `linuxkpi/include/linux/*.h` — compat headers (`kernel.h`, `types.h`, `compiler.h`, `slab.h`, `gfp.h`, `printk.h`, `string.h`, `err.h`, `list.h`, `kref.h`, `atomic.h`, `bitops.h`, `spinlock.h`, `mutex.h`, `completion.h`, `wait.h`, `jiffies.h`, `timer.h`, `delay.h`, `workqueue.h`, `interrupt.h`, `device.h`, `pci.h`, `dma-mapping.h`, `io.h`, `scatterlist.h`, `idr.h`, `xarray.h`, `dma-fence.h`, `module.h`, `sort.h`, …). Added on demand.
- `linuxkpi/include/asm/`, `linuxkpi/include/asm-generic/` — as the linker/compiler demands.
- `linuxkpi/compat.h` — force-included prelude (`-include`), global typedefs/macros.
- `linuxkpi/autoconf.h` — minimal `CONFIG_*` (`CONFIG_DRM`, `CONFIG_VIRTIO`, `CONFIG_VIRTIO_PCI`, `CONFIG_DRM_VIRTIO_GPU`, …).
- `linuxkpi/kpi_slab.c` — `kmalloc/kzalloc/kfree/krealloc/kmem_cache_*/vmalloc/__get_free_pages` over `knx_malloc/knx_free`.
- `linuxkpi/kpi_print.c` — `vsnprintf` (subset incl. `%pK/%pa/%px`), `printk/pr_*/dev_*`, `WARN/BUG` → `knx_log`.
- `linuxkpi/kpi_idr.c` — `idr/ida` (+ small `xarray` if needed).
- `linuxkpi/kpi_sg.c` — `scatterlist` helpers.
- `linuxkpi/kpi_sort.c` — `sort`, `bsearch`.
- `linuxkpi/kpi_time.c` — `jiffies`, timers, `msleep/udelay/usleep_range`, `ktime`.
- `linuxkpi/kpi_sync.c` — `mutex`, `spinlock`, `completion`, `wait_queue`.
- `linuxkpi/kpi_irq.c` — `request_irq/request_threaded_irq/free_irq`.
- `linuxkpi/kpi_workqueue.c` — workqueues (cooperative worker thread).
- `linuxkpi/kpi_pci.c` — `pci_*` → `knx_pci_*`.
- `linuxkpi/kpi_dma.c` — `dma_alloc_coherent/dma_map_*` → `knx_dma_alloc` (identity).
- `linuxkpi/kpi_device.c` — `struct device` helpers, `devm_*`.
- `linuxkpi/kpi_fence.c` — `dma_fence_*`.
- `linuxkpi/kpi_misc.c` — `kref`, `err`, `kstrdup`, refcount, misc.

**New — vendored Linux (GPLv2, foreign, compiled not modified):**
- `external/linux-6.12/COPYING`, `external/linux-6.12/VERSION-PROVENANCE`
- `external/linux-6.12/drivers/virtio/{virtio.c,virtio_ring.c,virtio_pci_modern*.c,…}`
- `external/linux-6.12/drivers/gpu/drm/{drm_drv.c,drm_managed.c,drm_atomic*.c,drm_crtc*.c,drm_plane*.c,drm_connector.c,drm_encoder.c,drm_probe_helper.c,drm_framebuffer.c,drm_fourcc.c,drm_gem.c,drm_gem_shmem_helper.c,drm_gem_framebuffer_helper.c,…}` (subset, grows link-error-driven)
- `external/linux-6.12/drivers/gpu/drm/virtio/*.c`
- `external/linux-6.12/include/...` — upstream headers the driver needs that are NOT kernel-API shims (e.g. `uapi/drm/*.h`, `linux/virtio_gpu.h`, `drm/*.h`). Kernel-API headers (`linux/slab.h` etc.) come from the shim, NOT here.

**New — module (our code):**
- `kext/virtio_gpu/virtio_gpu_kext.c` — `nkext_init()`: build the `virtio_device` for the PCI function, call driver probe, then start the bridge.
- `kext/virtio_gpu/drmfb_bridge.c` — `drm→fb0` bridge (force mode, atomic commit, repoint `Fb0Device`, dirty/flush).

**New — tooling/tests:**
- `scripts/vendor-linux.sh` — fetch+extract the needed 6.12 files.
- `tests/test_linuxkpi_slab.cpp`, `tests/test_linuxkpi_idr.cpp`, `tests/test_linuxkpi_sg.cpp`, `tests/test_linuxkpi_sort.cpp`, `tests/test_linuxkpi_print.cpp`, `tests/test_linuxkpi_time.cpp` — host doctest for pure primitives.

**Modified:**
- `Makefile` — `linuxkpi` compile rules, `external/linux-6.12` compile rules, `virtio_gpu.nkext` link rule, add `virtio_gpu` to `KEXTS`, add shim modules to `TEST_MODULES`/`COV_PATTERNS`, `verify64` gates.
- `kernel/kexports.def` — new `KX(...)` exports if needed (kernel-thread spawn, waitqueue) — only if a facility is missing.
- `kernel/KernelExports.cpp` + relevant `knx_*.h` — implementations for any new exports.
- `drivers/Fb0Device.{h,cpp}` / `drivers/Framebuffer.{h,cpp}` — allow repointing the backing store at a kext-provided scanout (bridge hook).

---

## Phase P0 — Build plumbing + loader proof + first primitives  ✅ COMPLETE

Goal: a `virtio_gpu.nkext` build path that compiles C-with-Linux-headers, links via `kext64.ld`, packs with `mknx64`, and loads under `loadAllKexts`; plus the first host-tested pure shim primitives. No vendored Linux yet — a trivial in-tree `.c` proves the toolchain.

### Task P0.1: Host-test the slab shim (kmalloc family)  ✅ DONE

**Files:**
- Create: `linuxkpi/include/linux/slab.h`, `linuxkpi/include/linux/gfp.h`, `linuxkpi/include/linux/types.h`, `linuxkpi/kpi_slab.c`
- Test: `tests/test_linuxkpi_slab.cpp`

**Interfaces:**
- Consumes: `knx_malloc(unsigned)`, `knx_free(void*)` (host shim forwards to libc, as existing tests do).
- Produces: `void *kmalloc(size_t,gfp_t)`, `void *kzalloc(size_t,gfp_t)`, `void kfree(const void*)`, `void *krealloc(const void*,size_t,gfp_t)`, `void *kcalloc(size_t,size_t,gfp_t)`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "doctest.h"
extern "C" {
#include "linux/slab.h"
}
TEST_CASE("kzalloc zeroes") {
    int *p = (int*)kzalloc(4*sizeof(int), 0);
    REQUIRE(p);
    for (int i=0;i<4;i++) CHECK(p[i]==0);
    kfree(p);
}
TEST_CASE("krealloc preserves") {
    char *p = (char*)kmalloc(4, 0); for(int i=0;i<4;i++) p[i]=(char)(i+1);
    p = (char*)krealloc(p, 8, 0);
    for(int i=0;i<4;i++) CHECK(p[i]==(char)(i+1));
    kfree(p);
}
```

- [ ] **Step 2: Run to verify it fails** — `make test` (or the host compile of this one file). Expected: FAIL (no `slab.h`).

- [ ] **Step 3: Implement `slab.h` + `kpi_slab.c`** — `kmalloc`→`knx_malloc`; `kzalloc`→ malloc+`memset 0`; `krealloc`→ alloc+copy+free (track size via a header word or use libc realloc semantics through a stored length prefix); `gfp_t` = `unsigned`; `__GFP_ZERO` honoured.

- [ ] **Step 4: Run to verify pass** — `make test`. Expected: PASS.

- [ ] **Step 5: Commit** — `git add linuxkpi/include/linux/{slab,gfp,types}.h linuxkpi/kpi_slab.c tests/test_linuxkpi_slab.cpp && git commit -m "linuxkpi: slab (kmalloc family) over knx heap + host tests"`

### Task P0.2: Host-test the print shim (vsnprintf subset)  ✅ DONE

**Files:** Create `linuxkpi/include/linux/printk.h`, `linuxkpi/kpi_print.c`; Test `tests/test_linuxkpi_print.cpp`
**Interfaces:** Produces `int vscnprintf(char*,size_t,const char*,va_list)`, `int snprintf(...)`, `printk`, `pr_info/pr_err/dev_*` macros.

- [ ] **Step 1: failing test** — assert `snprintf` handles `%d %x %s %p %px %pa %zu %lld` correctly (exact expected strings).
- [ ] **Step 2: run, expect FAIL.**
- [ ] **Step 3: implement** a self-contained `vsnprintf` covering `%d/u/x/X/p/px/pa/pK/s/c/zu/lu/llu/lld` and width/zero-pad; `printk`→ format then `knx_log` (host shim captures).
- [ ] **Step 4: run, expect PASS.**
- [ ] **Step 5: commit** — `linuxkpi: printk/vsnprintf subset + host tests`.

### Task P0.3: Host-test idr/ida  ✅ DONE

**Files:** Create `linuxkpi/include/linux/idr.h`, `linuxkpi/kpi_idr.c`; Test `tests/test_linuxkpi_idr.cpp`
**Interfaces:** Produces `int ida_alloc(struct ida*,gfp_t)`, `void ida_free(struct ida*,int)`, `int idr_alloc(struct idr*,void*,int,int,gfp_t)`, `void *idr_find(struct idr*,int)`, `void idr_remove(struct idr*,int)`.

- [ ] **Step 1: failing test** — alloc returns increasing ids, find returns the stored pointer, free recycles the id.
- [ ] **Step 2: run, expect FAIL.**
- [ ] **Step 3: implement** a growable array map (id→ptr), lowest-free-id allocation.
- [ ] **Step 4: run, expect PASS.**
- [ ] **Step 5: commit** — `linuxkpi: idr/ida + host tests`.

### Task P0.4: Host-test scatterlist + sort/bsearch  ✅ DONE

**Files:** Create `linuxkpi/include/linux/{scatterlist,sort}.h`, `linuxkpi/kpi_sg.c`, `linuxkpi/kpi_sort.c`; Test `tests/test_linuxkpi_sg.cpp`, `tests/test_linuxkpi_sort.cpp`
**Interfaces:** Produces `sg_init_table/sg_set_buf/sg_next/for_each_sg/sg_dma_address/sg_dma_len`; `void sort(void*,size_t,size_t,int(*)(const void*,const void*),void(*)(void*,void*,int))`, `bsearch`.

- [ ] **Step 1–5 (sg):** test build a 3-entry table, iterate, last has `sg_is_last`; implement; pass; commit.
- [ ] **Step 1–5 (sort):** test sort an int array desc/asc + bsearch hit/miss; implement (simple qsort/bsearch); pass; commit `linuxkpi: scatterlist + sort/bsearch + host tests`.

### Task P0.5: Host-test jiffies/time math  ✅ DONE

**Files:** Create `linuxkpi/include/linux/{jiffies,delay,timer}.h`, `linuxkpi/kpi_time.c`; Test `tests/test_linuxkpi_time.cpp`
**Interfaces:** Produces `jiffies` (global), `msecs_to_jiffies/jiffies_to_msecs`, `time_after/time_before`, `mod_timer/del_timer/timer_setup` (logic-testable parts). HZ=1000. `knx_uptime_us` drives `jiffies` at runtime; tests check the pure conversions + `time_after` wraparound.

- [ ] **Steps 1–5:** test conversions + `time_after` across u32 wrap; implement; pass; commit `linuxkpi: jiffies/time conversions + host tests`.

### Task P0.6: Build plumbing — compile a trivial Linux-style kext  ✅ DONE

**Files:** Modify `Makefile`; Create `kext/virtio_gpu/hello_kpi.c` (temporary probe), `linuxkpi/compat.h`, `linuxkpi/autoconf.h`
**Interfaces:** Produces a `virtio_gpu.nkext` (initially the hello stub) that loads at boot.

- [ ] **Step 1:** Add Makefile vars: `LINUXKPI_INC=-Ilinuxkpi/include -include linuxkpi/compat.h`, `LINUXKPI_CFLAGS=$(KEXT_CFLAGS) -D__KERNEL__ -include linuxkpi/autoconf.h $(LINUXKPI_INC) -std=gnu11 -Wno-unused -Wno-implicit-fallthrough`. Add a `$(BINFOLDER)%.o: linuxkpi/%.c` rule (uses `$(CC)` with `LINUXKPI_CFLAGS`) and `$(BINFOLDER)%.o: kext/virtio_gpu/%.c` rule.
- [ ] **Step 2:** Write `linuxkpi/compat.h` (typedefs: `u8..u64`, `__le*`, `bool`, `gfp_t`; common macros) and `linuxkpi/autoconf.h` (empty for now).
- [ ] **Step 3:** Write `kext/virtio_gpu/hello_kpi.c`: `extern "C"`-free C with `int nkext_init(void){ pr_info("virtio_gpu kpi: hello\n"); void*p=kmalloc(16,0); kfree(p); return 0; }` (uses the shim, proving headers + slab + print link).
- [ ] **Step 4:** Add `virtio_gpu.nkext` link rule: link `KEXT_GLUE` + `hello_kpi.o` + `kpi_slab.o kpi_print.o kpi_idr.o kpi_sg.o kpi_sort.o kpi_time.o` via `kext64.ld`, `mknx64` → `.nkext`. Add `virtio_gpu` to `KEXTS`.
- [ ] **Step 5:** `make ARCH=x86_64 bin/virtio_gpu.nkext` — Expected: builds clean.
- [ ] **Step 6:** `make ARCH=x86_64 image64 run64` headless; capture boot log. Expected: `kext: virtio_gpu [loaded]` and `virtio_gpu kpi: hello`.
- [ ] **Step 7: Commit** — `linuxkpi: build plumbing + hello kext loads (P0 proof)`.

**P0 checkpoint:** `virtio_gpu.nkext` loads and runs shim code at ring 0; pure primitives are host-tested and green.

---

## Phase P1 — virtio core: probe + virtqueue + GET_DISPLAY_INFO  ✅ COMPLETE

Goal: vendor virtio core, shim PCI/MMIO/DMA/IRQ/sync, hand-build a `virtio_device` for the virtio-gpu PCI function, drive negotiation + one virtqueue + a control-queue round-trip.

### Task P1.1: Vendor script + virtio core files

**Files:** Create `scripts/vendor-linux.sh`; populate `external/linux-6.12/...`
- [ ] **Step 1:** Write `scripts/vendor-linux.sh`: download `linux-6.12.tar.xz` from cdn.kernel.org, verify, extract only the path list (virtio core + needed `include/` + uapi headers), drop into `external/linux-6.12/`, write `VERSION-PROVENANCE` (url + sha256) and copy `COPYING`.
- [ ] **Step 2:** Run it; commit the vendored virtio core + headers (GPLv2). Commit `vendor: Linux 6.12 virtio core (GPLv2)`.

### Task P1.2: Shim PCI + MMIO + DMA + IRQ + sync (compile-driven)

**Files:** Create `linuxkpi/kpi_pci.c`, `kpi_dma.c`, `kpi_irq.c`, `kpi_sync.c`, `kpi_device.c`, `kpi_workqueue.c`, `kpi_misc.c` + their headers.
- [ ] **Step 1:** Add the virtio core `.c` to the `virtio_gpu.nkext` link list. `make bin/virtio_gpu.nkext`.
- [ ] **Step 2 (loop):** For each undefined symbol the linker reports, implement it in the matching `kpi_*.c`/header over NanOS primitives (`knx_pci_*`, `knx_map_mmio`, `knx_dma_alloc`, `knx_register_irq/msi`, spinlock/mutex/completion/wait). Re-link. Repeat until it links.
- [ ] **Step 3:** Add `KX(...)` exports for any missing kernel facility (kernel-thread spawn for threaded IRQ/workqueue, waitqueue wake) in `kexports.def` + implement in `KernelExports.cpp`.
- [ ] **Step 4:** Host-test the pure additions (completion signal/wait state machine, ida reuse) where feasible.
- [ ] **Step 5: Commit** — `linuxkpi: PCI/DMA/IRQ/sync/workqueue shim; virtio core links`.

### Task P1.3: Hand-build virtio_device + probe to GET_DISPLAY_INFO

**Files:** Modify `kext/virtio_gpu/virtio_gpu_kext.c` (replaces hello stub).
- [ ] **Step 1:** In `nkext_init`: `knx_pci_find(0x1AF4, 0x1050)` (virtio-gpu modern) → build `struct virtio_pci_device`/`virtio_device`, map BARs via shim, register the virtio-pci modern transport, call `register_virtio_device`-equivalent path so the bound driver (or our manual call) negotiates features and sets up vqs.
- [ ] **Step 2:** Send `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` on the control queue; on completion log the reported display rect via `pr_info`.
- [ ] **Step 3:** `make image64 run64` headless with `-device virtio-gpu-pci`; capture log. Expected: display-info width/height logged (QEMU default 1280x800).
- [ ] **Step 4: Commit** — `virtio_gpu: probe + control-vq GET_DISPLAY_INFO (P1 checkpoint)`.

**P1 checkpoint:** NWDBG/boot log shows virtio-gpu display info read over a real virtqueue.

---

## Phase P2 — DRM/GEM core + modeset + test pattern

Goal: vendor the DRM core subset, let `virtio_gpu` create a `drm_device` + resource + set a mode; bridge draws a test pattern; QEMU screendump shows it.

### Task P2.1: Vendor DRM core subset + driver

- [ ] **Step 1:** Extend `scripts/vendor-linux.sh` with the DRM core file list (§5 of the spec) + `drivers/gpu/drm/virtio/*` + DRM uapi/headers. Re-run; commit `vendor: Linux 6.12 DRM core subset + virtio_gpu driver (GPLv2)`.

### Task P2.2: Shim the DRM-pulled leaf APIs (compile-driven)

- [ ] **Step 1:** Add DRM core + driver `.c` to the link list. `make bin/virtio_gpu.nkext`.
- [ ] **Step 2 (loop):** Resolve each undefined symbol — extend `kpi_*` (kref, idr, xarray-if-needed, dma-fence, gem-shmem page provider backing pages with `knx_dma_alloc`, sort, string helpers). The **gem_shmem page provider** is the known-hard piece: implement a fake page allocator returning contiguous pages mapped to a `knx_dma_alloc` region. Re-link until it links.
- [ ] **Step 3:** Host-test the dma-fence signal/wait state machine and the page-provider bookkeeping where pure.
- [ ] **Step 4: Commit** — `linuxkpi: DRM core leaf shims (kref/fence/gem-shmem pages); driver links`.

### Task P2.3: Bridge — force mode + atomic commit + test pattern

**Files:** Create `kext/virtio_gpu/drmfb_bridge.c`; modify `virtio_gpu_kext.c`.
- [ ] **Step 1:** After probe, enumerate the connector, pick a mode = display-info preferred (fallback fixed), allocate a GEM framebuffer, atomic-commit plane→crtc→connector.
- [ ] **Step 2:** Map the GEM CPU address; fill it with a recognisable test pattern (color bars / checkerboard); call the driver dirty/flush (`TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`).
- [ ] **Step 3:** `make image64`, boot QEMU `-device virtio-gpu-pci -vga none -display none -monitor unix:...`, `screendump`, convert to PNG, read it. Expected: the test pattern is visible.
- [ ] **Step 4: Commit** — `virtio_gpu: drm modeset + test-pattern via bridge (P2 hard checkpoint)`.

**P2 checkpoint (hard):** QEMU screendump shows the bridge's test pattern rendered through virtio_gpu modeset.

---

## Phase P3 — `/dev/fb0` bridge: desktop on virtio-gpu

Goal: repoint `Fb0Device` at the virtio_gpu scanout and wire damage→flush so `fbcon`/`nwm` render through the Linux driver.

### Task P3.1: Fb0Device repoint hook

**Files:** Modify `drivers/Fb0Device.{h,cpp}`, `drivers/Framebuffer.{h,cpp}`; add a `KX(...)` export so a kext can register a scanout (address, pitch, w, h, bpp, flush-callback).
- [ ] **Step 1:** Add `knx_fb_set_backing(void* cpuAddr, uint32_t pitch, uint32_t w, uint32_t h, uint32_t bpp, void(*flush)(int x,int y,int w,int h))` to `kexports.def` + implement in `KernelExports.cpp` → updates the `Framebuffer`/`Fb0Device` backing + registers a damage→flush callback.
- [ ] **Step 2:** Host-test the Framebuffer repoint logic (backing swap + damage coalescing) where pure.
- [ ] **Step 3: Commit** — `fb: knx_fb_set_backing — let a kext provide the scanout`.

### Task P3.2: Bridge wires scanout into fb0

**Files:** Modify `kext/virtio_gpu/drmfb_bridge.c`.
- [ ] **Step 1:** Replace the test-pattern fill with `knx_fb_set_backing(gem_cpu, pitch, w, h, 32, virtio_flush)`, where `virtio_flush` issues `TRANSFER_TO_HOST_2D`+`RESOURCE_FLUSH` for the damaged rect.
- [ ] **Step 2:** Ensure mode = NanOS console/nwm resolution (or set the framebuffer console to the virtio-gpu mode).
- [ ] **Step 3:** `make image64`, boot to `nwm` (graphics VT F7), `screendump`, read PNG. Expected: the NanOS desktop renders via virtio_gpu.
- [ ] **Step 4: Commit** — `virtio_gpu: wire scanout into /dev/fb0 — desktop on Linux GPU driver (P3 done)`.

### Task P3.3: Gate it in verify64 + docs

**Files:** Modify `Makefile` (`verify64`), `docs/en/kext.md` + `docs/pl/kext.md` (LinuxKPI section), `docs/en/x86_64.md` if display arch noted.
- [ ] **Step 1:** Add a `smoke-virtio-gpu` gate: boot `-device virtio-gpu-pci -vga none`, assert kext loaded + screendump non-blank.
- [ ] **Step 2:** Document the LinuxKPI layer + virtio_gpu module + how to add the next Linux driver.
- [ ] **Step 3: Commit** — `verify64 + docs: LinuxKPI / virtio-gpu`.

**P3 checkpoint (DONE):** NanOS boots and `nwm` renders on `/dev/fb0` backed by the Linux `virtio_gpu` driver, verified by QEMU screendump; `verify64` green.

---

## Risks carried from the spec (watch during execution)

- **DRM lift larger than hoped** → link-error-driven; stub debugfs/sysfs/edid; one forced mode.
- **`drm_gem_shmem` page provider** (hardest) → fake contiguous page allocator over `knx_dma_alloc`.
- **`dma-fence`** → minimal signal-on-used shim.
- **`%p` printf variants** → implemented subset; extend if the driver uses more.
- **INTx vs MSI** → start INTx (`knx_pci_irq`); switch to `knx_register_msi` if INTx flakey in QEMU.
- **SMP safety** → use existing fine-grained locks under the spinlock shim.
- **vmalloc non-contiguity** → not required by virtio path (uses `dma_alloc_coherent`).

## Execution notes (Ralph loop)

- Track phase/task progress in this file's checkboxes; commit after every green step.
- A phase's checkpoint must be **verified** (host test green / boot log / screendump) before its tasks are checked off — evidence before assertion.
- The completion promise is true ONLY after P3.3 is verified green.
