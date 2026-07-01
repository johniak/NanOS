# Vulkan on NanOS (virtio-gpu venus) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vulkan 1.x apps run on NanOS in QEMU on the M4 through the venus protocol (guest Mesa `vn` driver → virtio-gpu blob resources → host virglrenderer → KosmicKrisp/Metal), gated first by a host-stack validation with a stock Linux guest; ANV (Dell) interfaces are recorded but not implemented here.

**PREREQUISITE:** `2026-07-01-plan-1-gl-desktop-virgl-qemu.md` is fully implemented (DRM nodes + `mmapAt` + libdrm + Mesa port flow + smoke-gate pattern all exist). This plan only ADDS to that foundation.

**Architecture:** Venus serializes the entire Vulkan API into virtio-gpu execbuffers; the kernel's job is plumbing, not GL semantics: blob resources (`RESOURCE_CREATE_BLOB`), mapping host-allocated memory into the guest (`VIRTGPU_MAP` on blob BOs backed by the device's hostmem PCI BAR — driver file `virtgpu_vram.c`), per-context capset init (`CONTEXT_INIT`), and DRM syncobjs for fences. Userspace is Mesa's `vn` Vulkan driver + the Khronos loader. Design record: `docs/superpowers/specs/2026-07-01-gpu-stack-gl-vulkan-dell-design.md`.

**Tech Stack:** QEMU + virglrenderer with Venus (startergo kosmickrisp tap; macOS 15+, Apple Silicon), Mesa 25.1+ (`-Dvulkan-drivers=virtio`), Vulkan-Loader (static), vendored `virtgpu_drm.h` UAPI, doctest + QEMU smoke gates.

## Global Constraints

- All constraints of the GL plan apply verbatim (x86_64-only, real DRM ABI only, GPL segregation, CPU-fallback desktop untouched, no Claude attribution in commits, host-test pure logic / smoke-test device paths, gates SKIP when the venus QEMU is absent).
- **Hard GO/NO-GO gate at Task 1:** if venus does not work with an *emulated x86_64 stock-Linux guest* on the M4 host, STOP the plan and record findings — do not burn weeks making NanOS venus-ready against a host stack that cannot run it. (Venus+TCG is the plan's single biggest external risk; all documented venus setups use KVM/HVF.)
- The desktop stays on GL. Vulkan's success criterion here is offscreen-verified rendering (vkcube-style), not presentation/WSI.

---

## File Structure

**Created:**
- `scripts/run64-vk.sh` — boot with venus enabled (`-device virtio-gpu-gl,venus=true,blob=true,hostmem=512M` + memfd memory backend; exact flags per tap docs, see Task 1).
- `scripts/smoke-venus-host.sh` — Task-1 host validation driver (stock Linux guest), NOT wired into verify64 (manual gate).
- `scripts/smoke-venus.sh` — NanOS venus smoke gate (wired into verify64, SKIPs without venus QEMU).
- `kext/virtio_gpu/virtio_gpu_hostmem.c` — hostmem BAR discovery + blob mmap-offset resolution.
- `user/vktri/vktri.c` — offscreen Vulkan triangle + readback oracle (no WSI).
- `tests/test_hostmem_map.cpp` — host doctest for blob-offset routing logic.
- `$(SDK_WORK)/mesa-port/` gains a second build dir (`build-vk`) — same tree, Vulkan matrix.
- `$(SDK_WORK)/vulkan-loader-port/` — Khronos loader static port.

**Modified:**
- `kext/virtio_gpu/virtio_gpu_drv_entry.c` — negotiate/log `VIRTIO_GPU_F_RESOURCE_BLOB` + `F_CONTEXT_INIT`.
- `kext/virtio_gpu/virtio_gpu_drm_node.c` — extend `node_mmap_offset` to blob/vram objects.
- `linuxkpi/` — whatever `virtgpu_vram.c`'s io-remap path needs (it is already compiled; its mmap path is what we activate).
- `Makefile` — `mesa-vk` / `vulkan-loader` targets, `vktri.nxe`, smoke wiring.
- `docs/en/graphics.md` + `docs/pl/graphics.md` — venus section; `docs/superpowers/specs/2026-07-01-gpu-stack-gl-vulkan-dell-design.md` — status updates.

---

### Task 1: Host-stack validation — venus with a stock Linux guest (GO/NO-GO)

Prove the M4 host can do venus at all, with zero NanOS variables: an Arch/Alpine live ISO whose Mesa is ≥ 25.1, on the SAME qemu binary and flags NanOS will use. Everything after this task builds on a proven host.

**Files:**
- Create: `scripts/run64-vk.sh`, `scripts/smoke-venus-host.sh` (manual, documents the procedure)

**Interfaces:**
- Produces: `run64-vk.sh` — identical to `run64-gl.sh` but with the venus device stanza; `IMG=` overridable so the same script boots the Linux ISO (validation) and `image64` (later tasks).

- [ ] **Step 1: Write `run64-vk.sh`**

Copy `scripts/run64-gl.sh`; replace the device/display stanza with (exact property names from the tap's README — verify with `"$QEMU_GL" -device virtio-gpu-gl,help` and `qemu-system-x86_64 -object help | grep memfd`):

```bash
exec "$QEMU_GL" -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 2048 \
    -object memory-backend-memfd,id=mem1,size=2048M -machine memory-backend=mem1 \
    -drive file="${IMG}",format=raw \
    -device virtio-gpu-gl,venus=true,blob=true,hostmem=512M \
    -display "${DISPLAY_BACKEND:-cocoa,gl=es}" \
    -serial file:"$SER" -monitor unix:/tmp/nanos-vk-qmon.sock,server,nowait -no-reboot "$@"
```

Notes: memfd backend is REQUIRED for blob mapping; if the tap spells the device `virtio-gpu-pci,vulkan=on,hostmem=…` (its README shows that form), use the tap's spelling and record it in the script comment. Keep `-m` ≥ 2G: hostmem is carved beyond guest RAM.

- [ ] **Step 2: Boot a stock Linux guest and probe Vulkan**

```bash
curl -sLo /tmp/arch.iso 'https://geo.mirror.pkgbuild.com/iso/latest/archlinux-x86_64.iso'
IMG=/dev/null bash scripts/run64-vk.sh -cdrom /tmp/arch.iso -boot d
# In the guest (interactive, cocoa window): expect /dev/dri to exist, then:
#   pacman -Sy --noconfirm vulkan-tools mesa   (live env has network)
#   vulkaninfo --summary
#   vkcube --c 600
```

Expected (GO): `vulkaninfo` lists a device named `Virtio-GPU Venus (…)`; `vkcube` renders in the guest window. TCG x86_64 will be slow (minutes to boot) — patience, not failure.
Expected (NO-GO variants + what they mean): no `venus` property on the device → tap too old, try the tap's docs/issues; `vulkaninfo` shows only llvmpipe → host virglrenderer built without venus or KosmicKrisp missing; guest kernel lacking `virtio_gpu` blob support → wrong ISO (use current Arch). If after ≤ 1 day of tap-level debugging venus does not light up under TCG: **STOP THE PLAN**, write findings into `docs/superpowers/specs/2026-07-01-gpu-stack-gl-vulkan-dell-design.md` §2 (venus/TCG status), and re-evaluate (options recorded there: wait for tap/QEMU venus-on-TCG fixes, or shelve Vulkan until the Dell/ANV path).

- [ ] **Step 3: Record the validated invocation**

Write `scripts/smoke-venus-host.sh` as the executable record of Step 2 (boot ISO, print the two manual commands + expected outputs) with a header comment stating the validated qemu version (`"$QEMU_GL" --version`), date, and result.

- [ ] **Step 4: Commit**

```bash
git add scripts/run64-vk.sh scripts/smoke-venus-host.sh
git commit -m "vk: venus-capable QEMU run script + validated host stack (stock Linux guest)"
```

---

### Task 2: Kernel — negotiate blob/context-init features + hostmem BAR

Enable the driver's blob-resource machinery: negotiate the two features, and surface the hostmem region (a PCI BAR on the virtio-gpu device) so blob mappings can be resolved to physical addresses.

**Files:**
- Modify: `kext/virtio_gpu/virtio_gpu_drv_entry.c`
- Create: `kext/virtio_gpu/virtio_gpu_hostmem.c`
- Modify: `Makefile` (`VIRTIO_GPU_OBJS += virtio_gpu_hostmem.o`)

**Interfaces:**
- Consumes: the vendored driver's feature handling (`virtio_gpu_kms.c` reads `VIRTIO_GPU_F_RESOURCE_BLOB`/`F_CONTEXT_INIT` into `vgdev->has_resource_blob`/`has_context_init` — confirm field names by reading `external/linux-6.12/drivers/gpu/drm/virtio/virtgpu_kms.c` first); virtio-pci shared-memory capability parsing (`VIRTIO_PCI_CAP_SHARED_MEMORY_CFG`) — the hostmem region is virtio shm region id `VIRTIO_GPU_SHM_ID_HOST_VISIBLE` (see `virtgpu_drv.c`'s `virtio_get_shm_region` call).
- Produces: `int virtio_gpu_hostmem_init(struct virtio_device *vdev)` — locates the host-visible shm region (phys base + len) via the transport (`kext/virtio_gpu/virtio_transport.c` must implement `virtio_get_shm_region` if it doesn't yet — that is THE expected gap; implement it by parsing the PCI vendor capabilities per virtio spec §4.1.4.7, using `knx_pci_cfg_read32`); exports `int virtio_gpu_hostmem_range(uint64_t *phys, uint64_t *len)`. Serial markers: `virtio_gpu: blob resources negotiated`, `virtio_gpu: hostmem @%llx +%llx`, or `virtio_gpu: no blob/hostmem (venus off)`.

- [ ] **Step 1: Read the two source files (30 min, take notes in the new file's header):** `virtgpu_kms.c` (feature fields, `virtio_gpu_init`'s shm lookup) and `virtio_transport.c` (our transport surface — what exists for capabilities today).

- [ ] **Step 2: Implement `virtio_get_shm_region` in the transport** (if absent): walk PCI capability list (cap id 0x09, cfg_type `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG`=8), match `id == shmid`, read the 64-bit offset/length, resolve against the BAR base (`knx_pci_bar`). ~60 lines. Host-test the pure parsing with a doctest over a synthetic capability blob if the transport layer is host-testable (mirror how existing transport code is tested; if it isn't, keep the parser a pure function `int virtio_pci_parse_shm(const uint8_t *cfg, unsigned len, int shmid, uint64_t *off, uint64_t *blen, int *bar)` in a host-testable file and test THAT).

- [ ] **Step 3: Wire + boot on the venus QEMU**

`virtio_gpu_drv_entry.c`: after negotiation, log the three markers.
Run: `make image64 && IMG=disk/image64-grub2.img SER=/tmp/vk.log DISPLAY_BACKEND=none bash scripts/run64-vk.sh & sleep 60; grep -E "blob resources|hostmem @" /tmp/vk.log; pkill -9 -f image64`
Expected: both markers, sane hostmem range (512M). On the GL (non-venus) QEMU: `no blob/hostmem` and everything else still green (`make smoke-virtio-gpu-gl`).

- [ ] **Step 4: Commit**

```bash
git add kext/virtio_gpu/ linuxkpi/ Makefile tests/
git commit -m "virtio_gpu: negotiate blob/context-init + locate the host-visible shm region (venus plumbing)"
```

---

### Task 3: Kernel — blob mmap path (`VIRTGPU_MAP` on blob BOs)

Venus maps host-allocated memory: `DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB` (handled by the driver already — it's in `virtgpu_ioctl.c`) then `DRM_IOCTL_VIRTGPU_MAP` + `mmap(fd, offset)`. Blob BOs are `virtgpu_vram.c` objects whose backing is a *carve-out of the hostmem BAR*, not guest pages — extend `node_mmap_offset` to resolve them.

**Files:**
- Modify: `kext/virtio_gpu/virtio_gpu_drm_node.c` (`node_mmap_offset`)
- Create: `tests/test_hostmem_map.cpp` (pure routing logic)
- Modify: `linuxkpi/` as link errors demand (the vram object path may pull `io_remap_pfn_range`-adjacent helpers; shim to identity map — our device mmap is phys-based anyway, we only need the BO's `vram->vram_node.start` offset into the BAR)

**Interfaces:**
- Consumes: `struct virtio_gpu_object_vram` (`virtgpu_drv.h`: contains a `struct drm_mm_node vram_node` whose `.start` is the offset inside the hostmem region — VERIFY by reading `virtgpu_vram.c:virtio_gpu_vram_map`), `virtio_gpu_hostmem_range()` (Task 2).
- Produces: `node_mmap_offset` returns, for a vram/blob BO: **`vram_node.start` is already an ABSOLUTE address** — the driver initializes the allocator with `drm_mm_init(&vgdev->host_visible_mm, region.addr, region.len)` (`virtgpu_kms.c:192-194`) and its own mmap path maps `vram_node.start >> PAGE_SHIFT` directly as the PFN (`virtgpu_vram.c:63-64`). So: `*phys = vram->vram_node.start` (do NOT add `hostmem_base` — that would double-count the region base), `*len = obj size`, with a bounds assert `vram_node.start >= hostmem_base && vram_node.start + size <= hostmem_base + hostmem_len` (fail loud on violation). For shmem BOs: unchanged Task-5 (GL plan) behavior. Distinguish via the driver's own type test (`virtio_gpu_is_vram(bo)` — exists in `virtgpu_drv.h`; confirm).

- [ ] **Step 1: Write the failing routing doctest** — factor the "shmem vs vram → phys" decision into a pure function `int vgpu_bo_phys(int is_vram, uint64_t shmem_page0_phys, uint64_t hostmem_base, uint64_t hostmem_len, uint64_t vram_start_abs, uint64_t size, uint64_t *phys)` in a host-compilable header and assert: vram branch returns `vram_start_abs` verbatim; the bounds guard rejects `vram_start_abs < hostmem_base` and `vram_start_abs + size > hostmem_base + hostmem_len`; shmem branch returns `shmem_page0_phys`.

- [ ] **Step 2: Implement in `node_mmap_offset`** (vma lookup as in the GL plan Task 5, then branch by `virtio_gpu_is_vram`).

- [ ] **Step 3: In-guest oracle — extend `drmtest` with a blob subtest** (only runs when GETPARAM `VIRTGPU_PARAM_RESOURCE_BLOB` reports 1): create a small `VIRTGPU_BLOB_MEM_HOST3D` blob (guest-visible flags: `VIRTGPU_BLOB_FLAG_USE_MAPPABLE`), `VIRTGPU_MAP` it, mmap, write+read a pattern. Serial marker `drmtest: blob-map OK`.

Run on venus QEMU: marker present. Run `make verify64` + `smoke-virtio-gpu-gl`: green (no regression).

- [ ] **Step 4: Commit**

```bash
git add kext/virtio_gpu/ user/drmtest/ tests/ linuxkpi/
git commit -m "virtio_gpu: blob-BO mmap via hostmem BAR (venus memory path) + drmtest blob oracle"
```

---

### Task 4: Kernel — syncobj sanity for venus fencing

Venus uses DRM syncobjs (incl. timelines) for `VkFence`/`VkSemaphore`. `drm_syncobj.c` is compiled (stubs added during the DRM lift) — this task proves the four core ioctls work end-to-end and fixes what the stubs faked.

**Files:**
- Modify: `user/drmtest/drmtest.c` (syncobj subtest), `linuxkpi/` (real eventfd-free wait: syncobj waits must block on dma_fence signal via the cooperative pump — audit `linuxkpi/kpi_fence.c` wait hooks)

**Interfaces:**
- Produces: drmtest markers `drmtest: syncobj OK` — sequence: `SYNCOBJ_CREATE` → `SYNCOBJ_HANDLE_TO_FD`? (skip fd export — no cross-process need) → submit an EXECBUFFER with `VIRTGPU_EXECBUF_FENCE_FD_OUT`? — venus actually drives fencing through execbuffer fence + `SYNCOBJ_TIMELINE_WAIT`. Read `mesa-25.x/src/virtio/vulkan/vn_queue.c` (in the mesa-port tree) FIRST and list the exact ioctl set it emits; test exactly that set, nothing more. Record the list in the drmtest comment.

- [ ] **Step 1: Extract the ioctl list from `vn_queue.c`/`vn_device.c` (grep `DRM_IOCTL_` under `src/virtio/vulkan/`).** Record.
- [ ] **Step 2: drmtest subtest exercising that list against the venus QEMU;** fix linuxkpi stubs it exposes (each fix = real semantics via existing kpi_fence machinery, no fakes).
- [ ] **Step 3: `make verify64` + GL smoke green; commit** `git commit -m "virtio_gpu: syncobj path venus needs, proven by drmtest"`.

---

### Task 5: Mesa `vn` + Vulkan loader ports

Second Mesa build config from the same port tree (Mesa ≥ 25.1 — venus without KVM-era patches; the GL build stays pinned as-is), plus the Khronos static loader, plus `vktri.nxe`.

**Files:**
- Create (external): `$(SDK_WORK)/mesa-port/hooks/pre_configure_vk.sh`; `$(SDK_WORK)/vulkan-loader-port/` (nxport.toml + cross file, static build, `BUILD_TESTS=OFF`)
- Create: `user/vktri/vktri.c`
- Modify: `Makefile` (targets `mesa-vk`, `vulkan-loader`, `vktri.nxe`)

**Interfaces:**
- Produces: `libvulkan_virtio.a` (the vn ICD, static) + `libvulkan.a` (loader) + `vktri.nxe`. **Loader note:** the loader's dlopen-based ICD discovery cannot work statically — link vn directly and use `vkGetInstanceProcAddr` from the ICD via the loader's static-ICD mode, or skip the loader entirely for vktri (`vn`'s `vk_icdGetInstanceProcAddr` + a 30-line dispatch shim). Decide by reading how Mesa 25.x supports `-Dvulkan-icd-dir` static consumers; prefer the simplest honest path (direct ICD link) and record the loader as follow-on if unused.

- [ ] **Step 1: Mesa vk build**

```bash
# hooks/pre_configure_vk.sh — Mesa >= 25.1 tree (fetch + pin mesa-25.1.x alongside 24.2)
meson setup build-vk src-25 --cross-file /work/port/cross-nanos.ini \
  -Dplatforms= -Dgallium-drivers= -Dvulkan-drivers=virtio \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles2=disabled \
  -Dllvm=disabled -Dshader-cache=disabled -Dzstd=disabled -Dexpat=disabled \
  -Dxmlconfig=disabled -Ddefault_library=static --buildtype=debugoptimized
ninja -C build-vk
```

Expected gaps: vn needs `timerfd`/`eventfd`? (vn uses them for some paths — stub honestly in libc-glue with ENOSYS and verify vn's fallback engages, or implement over existing NanOS primitives if vn hard-requires); `VN_DEBUG` env honored — useful later.

- [ ] **Step 2: `vktri.c` — offscreen triangle + readback**

The shaders, compiled OFFLINE on the mac host (record both commands verbatim in the file header comment):

```glsl
// tri.vert — glslangValidator -V tri.vert -o tri.vert.spv && xxd -i tri.vert.spv
#version 450
layout(location = 0) out vec3 v_col;
vec2 P[3] = vec2[](vec2(-0.9, 0.9), vec2(0.9, 0.9), vec2(0.0, -0.9));
void main() { gl_Position = vec4(P[gl_VertexIndex], 0, 1); v_col = vec3(0, 1, 0); }

// tri.frag — glslangValidator -V tri.frag -o tri.frag.spv && xxd -i tri.frag.spv
#version 450
layout(location = 0) in vec3 v_col;
layout(location = 0) out vec4 o_col;
void main() { o_col = vec4(v_col, 1.0); }
```

Paste the two `xxd -i` arrays into `vktri.c` as `vs_spv[]`/`fs_spv[]`. The program (complete; every call checked by the `CK` macro which prints the failing line + returns 1):

```c
/* vktri.nxe — offscreen Vulkan triangle over venus; no WSI, readback oracle. */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
/* vs_spv[] / fs_spv[] pasted here (see header comment for the generating commands) */

#define CK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("vktri: FAIL %s = %d (line %d)\n", #x, (int)_r, __LINE__); return 1; } } while (0)
#define W 64
#define H 64

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int main(void)
{
    /* instance + device (queue family 0 must support graphics on vn — assert it) */
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst; CK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1; VkPhysicalDevice pd;
    CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("vktri: device=%s\n", props.deviceName);   /* expect ...Venus... */
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, 0);
    VkQueueFamilyProperties qf[8]; if (qn > 8) qn = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qi = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
    if (qi == UINT32_MAX) { printf("vktri: FAIL no gfx queue\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qi, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; CK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    /* color image (device-local) + readback buffer (host-visible) */
    VkImageCreateInfo imi = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; CK(vkCreateImage(dev, &imi, 0, &img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(pd, mr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; CK(vkAllocateMemory(dev, &mai, 0, &imem));
    CK(vkBindImageMemory(dev, img, imem, 0));

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf; CK(vkCreateBuffer(dev, &bci, 0, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem; CK(vkAllocateMemory(dev, &mai, 0, &bmem));
    CK(vkBindBufferMemory(dev, buf, bmem, 0));

    /* render pass + framebuffer + pipeline */
    VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sp };
    VkRenderPass rp; CK(vkCreateRenderPass(dev, &rpi, 0, &rp));
    VkImageViewCreateInfo ivi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView iv; CK(vkCreateImageView(dev, &ivi, 0, &iv));
    VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp, .attachmentCount = 1, .pAttachments = &iv,
        .width = W, .height = H, .layers = 1 };
    VkFramebuffer fb; CK(vkCreateFramebuffer(dev, &fbi, 0, &fb));

    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof vs_spv, .pCode = (const uint32_t *)vs_spv };
    VkShaderModule vs; CK(vkCreateShaderModule(dev, &smi, 0, &vs));
    smi.codeSize = sizeof fs_spv; smi.pCode = (const uint32_t *)fs_spv;
    VkShaderModule fs; CK(vkCreateShaderModule(dev, &smi, 0, &fs));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vin = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &ba };
    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkGraphicsPipelineCreateInfo gpi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vin,
        .pInputAssemblyState = &ia, .pViewportState = &vps,
        .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
    VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, 0, 1, &gpi, 0, &pipe));

    /* record + submit + wait */
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .queueFamilyIndex = qi };
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, 0, &cp));
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cbi, &cmd));
    VkCommandBufferBeginInfo bgi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CK(vkBeginCommandBuffer(cmd, &bgi));
    VkClearValue clear = { .color = { { 0, 0, 0, 1 } } };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } },
        .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy cp2 = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                              .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp2);
    CK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CK(vkCreateFence(dev, &fci, 0, &fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CK(vkQueueSubmit(q, 1, &si, fence));
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000));

    /* readback oracle: >=25% of pixels are the triangle green */
    void *p; CK(vkMapMemory(dev, bmem, 0, W * H * 4, 0, &p));
    unsigned green = 0; const unsigned char *px = p;
    for (int i = 0; i < W * H; i++)
        if (px[i * 4 + 0] < 50 && px[i * 4 + 1] > 200 && px[i * 4 + 2] < 50) green++;
    printf(green >= (W * H) / 4 ? "vktri: triangle OK (%u green)\n"
                                : "vktri: triangle BAD (%u green)\n", green);
    return green >= (W * H) / 4 ? 0 : 1;
}
```

Markers: `vktri: device=<name>` (must contain `Venus`), `vktri: triangle OK`.

- [ ] **Step 3: Run on venus QEMU**

Guest: `vktri` → both markers. `VN_DEBUG=all` on failure — venus logs the serialized command stream; the classic first failure is blob mmap (Task 3) or syncobj wait (Task 4) — both already gated, so failures here should be port-level (libc symbols), fix in libc-glue per the notes.md discipline.

- [ ] **Step 4: Commit** `git commit -m "ports: Mesa vn (venus Vulkan) + vktri offscreen oracle"`.

---

### Task 6: `smoke-venus` gate + docs (+ recorded follow-ons)

**Files:**
- Create: `scripts/smoke-venus.sh`; Modify: `Makefile` (`smoke-venus` + verify64), docs (en/pl graphics.md venus section), spec status.

- [ ] **Step 1: Gate:** SKIP unless `run64-vk.sh`'s qemu exists AND `-device virtio-gpu-gl,help` lists `venus`; else boot image64 via `run64-vk.sh`, assert serial: `blob resources negotiated`, `drmtest: blob-map OK`, `drmtest: syncobj OK`, `vktri: triangle OK`, no PANIC. Wire `smoke-venus` into `verify64:`.
- [ ] **Step 2:** `make verify64` all green (GL gates unaffected).
- [ ] **Step 3: Docs:** venus section in graphics.md (host requirements macOS 15+/M-series, tap, run64-vk, the GO/NO-GO record, `VN_DEBUG`); record follow-ons: WSI/presentation (Vulkan→screen via GBM or a compositor swapchain), zink (GL-over-Vulkan — worth a spike once vn is stable), **ANV on the Dell: same DRM-node + syncobj surface, i915 kernel driver from plan `2026-07-01-plan-3-gl-on-dell-i915-iris.md`, Mesa `-Dvulkan-drivers=intel`; no NanOS-side ABI additions expected beyond that plan.**
- [ ] **Step 4: Commit** `git commit -m "ci+docs: smoke-venus gate + venus documentation"`.

---

## Self-Review

**Coverage vs design record:** host GO/NO-GO before guest work (the TCG risk) → Task 1; venus plumbing = blob+hostmem+map+syncobj, driver code unmodified → Tasks 2-4; guest driver = Mesa vn ≥25.1 (matches the tap's stated guest floor) → Task 5; desktop unaffected, offscreen success criterion → vktri; ANV recorded as interfaces only. ✓

**Placeholder scan:** Task 4 deliberately derives its ioctl list from `vn_queue.c` instead of hardcoding a possibly-wrong list — that is a discovery step with a defined method, not a TBD. SPIR-V words generated offline with a recorded command (same pattern as the old plan's TGSI tokens). ✓

**Type consistency:** reuses `knx_drm_ops.mmap_offset` (GL plan) unchanged; new kext export `virtio_gpu_hostmem_range(uint64_t*, uint64_t*)` used in Tasks 2-3 consistently. ✓
