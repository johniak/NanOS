# GPU-Accelerated Desktop — Phase 1 (virtio-gpu / virgl) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Composite the `nwm` desktop — wallpaper, window textures, and the glass backdrop blur — on the GPU through the unmodified Linux `virtio_gpu` DRM driver's 3D (virgl) path, with the CPU compositor kept as a runtime fallback.

**Architecture:** Userspace builds virgl command buffers (`user/libnwgl`); a bespoke `/dev/vgpu` char device ferries them to the kext, which calls the unmodified driver's `virtgpu_vq.c` 3D helpers; `nw_compose` gains a GPU backend selected at runtime when a virgl-capable virtio-gpu is probed, otherwise the existing CPU path runs unchanged.

**Tech Stack:** C (kext + libnwgl + nwm), C++ (kernel CharDevice glue), TGSI shaders, virgl protocol (vendored `virgl_protocol.h` / `virgl_hw.h`), QEMU `virtio-vga-gl` + virglrenderer, doctest host tests, `screendump` smoke gates.

## Global Constraints

- **x86_64 only** — desktop/nwm/Linux drivers are 64-bit; kext links `kext64.ld`. Build via `make image64`; verify via `verify64`.
- **GPU API is OpenGL/virgl** — no Vulkan/venus anywhere in this plan.
- **The CPU `nw_compose` path stays fully functional** — same `nwm` binary must boot the desktop on the real Dell (firmware fb, no virtio-gpu) and on un-virgl'd QEMU. The GPU backend is selected only when `VGPU_PROBE` reports virgl present; any GPU submit error falls back to CPU for the session, never crashes the desktop.
- **Drive the device only through the unmodified driver's helpers** (`virtio_gpu_cmd_*` in `external/linux-6.12/.../virtgpu_vq.c`) — no hand-rolled virtio-gpu protocol in the kext, mirroring `kext/virtio_gpu/virtio_gpu_present.c`.
- **GPL segregation** — vendored Linux/virgl source links only into `virtio_gpu.nkext`, never into `kernel.bin`.
- **Commits/PRs contain NO mention of Claude** — no `Co-Authored-By`, no "Generated with" trailer.
- **Verification model:** pure-data code (the virgl encoder, ioctl arg marshalling) is host-tested with doctest under the `nanos-test` image; everything that touches the device or the GPU is verified by a QEMU `screendump` smoke gate — the established pattern for `virtio_gpu` (`smoke-virtio-gpu`) and hardware code in this repo.

---

## File Structure

**Created:**
- `scripts/run64-gl.sh` — boot `image64` with a virgl-capable QEMU (`virtio-vga-gl`).
- `scripts/smoke-virtio-gpu-gl.sh` — the GL display smoke gate (3D context up + GPU-composited desktop).
- `kext/virtio_gpu/virtio_gpu_3d.c` — K1: virgl feature negotiation, capset fetch, 3D context create; K2 op implementations calling the driver's 3D helpers.
- `kext/virtio_gpu/virtio_gpu_vgpu.h` — the `knx_vgpu_ops` table + ioctl command numbers + arg structs shared by kext and kernel.
- `drivers/VgpuDevice.h` / `drivers/VgpuDevice.cpp` — the `/dev/vgpu` `CharDevice`; forwards ioctls to the registered `knx_vgpu_ops`.
- `user/libnwgl/nwgl.h` / `nwgl.c` — the minimal virgl emitter (command-buffer encoder + resource/texture/present API over `/dev/vgpu`).
- `user/libnwgl/nwgl_encode.h` / `nwgl_encode.c` — pure-data virgl command-buffer builder (host-tested).
- `user/libnwgl/shaders.h` — the two shaders' TGSI token arrays (generated offline from the readable TGSI in this plan).
- `user/gltest/gltest.c` — a standalone `.nxe` that draws a textured quad (shader/encoder bring-up oracle).
- `external/virgl/virgl_protocol.h`, `external/virgl/virgl_hw.h` — vendored virgl protocol constants.
- `tests/test_nwgl_encode.cpp` — host doctest for the encoder.

**Modified:**
- `kext/virtio_gpu/virtio_gpu_drv_entry.c:148` — after `virtio_gpu_fbcon_bringup`, call `virtio_gpu_3d_init(vdev)`.
- `kernel/KernelExports.cpp` / `kernel/KernelExports.h` — add `knx_vgpu_register(const knx_vgpu_ops*)`, create `/dev/vgpu`.
- `linuxkpi/lkpi_knx.h` — declare `knx_vgpu_register` for the kext side.
- `user/nwm/nw_compose.c` — add the GPU backend + runtime selection; CPU path untouched.
- `Makefile` — `VIRTIO_GPU_OBJS += virtio_gpu_3d.o`; build `libnwgl` + `gltest.nxe`; add `TEST_MODULES`/`COV_PATTERNS` for `nwgl_encode`; add `smoke-virtio-gpu-gl`; wire into `verify64`.
- `docs/en/graphics.md` + `docs/pl/graphics.md` — document the virgl 3D path; `docs/en/README.md` index.

---

### Task 1: virgl-capable QEMU on the dev host + run script

A virgl QEMU is the prerequisite for every later verification. The Homebrew QEMU 10.2.0 lacks virglrenderer (`virtio-gpu-gl-pci` not found). This task produces a working GL QEMU invocation and a script, with no NanOS code change.

**Files:**
- Create: `scripts/run64-gl.sh`

**Interfaces:**
- Produces: `scripts/run64-gl.sh` boots `disk/image64-grub2.img` with `-device virtio-vga-gl` on a virgl-capable QEMU, serial mirrored to a log; `qemu-system-x86_64 -device virtio-vga-gl,help` succeeds.

- [ ] **Step 1: Install virglrenderer and a GL-enabled QEMU**

```bash
brew install virglrenderer
# Homebrew's qemu is built without virglrenderer; build one with it:
brew install --build-from-source --HEAD qemu --with-virglrenderer 2>/dev/null || \
  echo "If the bottle lacks virgl, build QEMU from source: ./configure --enable-virglrenderer --enable-opengl"
```

- [ ] **Step 2: Confirm the GL device exists**

Run: `qemu-system-x86_64 -device virtio-vga-gl,help`
Expected: prints the device's properties (notably `gl`), NOT "Device 'virtio-vga-gl' not found".

- [ ] **Step 3: Write the run script**

```bash
#!/usr/bin/env bash
# run64-gl.sh — boot image64 with virgl 3D acceleration (virtio-vga-gl + virglrenderer).
set -u
IMG=disk/image64-grub2.img
SER=${SER:-/tmp/nanos-gl.log}
[ -f "$IMG" ] || { echo "run 'make image64' first"; exit 2; }
exec qemu-system-x86_64 -accel tcg,thread=multi -cpu qemu64 -smp 1 -m 512 \
    -drive file="$IMG",format=raw \
    -device virtio-vga-gl -display "${DISPLAY_BACKEND:-cocoa,gl=on}" \
    -serial file:"$SER" -monitor unix:/tmp/nanos-gl-qmon.sock,server,nowait -no-reboot "$@"
```

- [ ] **Step 4: Verify it boots to the desktop on the existing 2D path**

Run: `make image64 && SER=/tmp/nanos-gl.log DISPLAY_BACKEND=none bash scripts/run64-gl.sh & sleep 45; grep -c "nanos login:" /tmp/nanos-gl.log; pkill -9 -f "qemu-system-x86_64.*image64"`
Expected: `1` (boots; the virtio_gpu driver still uses its current 2D scanout — 3D is added in Task 2).

- [ ] **Step 5: Commit**

```bash
git add scripts/run64-gl.sh
git commit -m "build: virgl-capable QEMU run script (virtio-vga-gl) for GPU-accel dev"
```

---

### Task 2: K1 — negotiate virgl, fetch capset, create a 3D context

Enable the device's 3D feature in the kext and create one virgl context at bringup, using only the unmodified driver's helpers. On a non-GL QEMU the feature is absent and the existing 2D path is untouched.

**Files:**
- Create: `kext/virtio_gpu/virtio_gpu_3d.c`
- Create: `kext/virtio_gpu/virtio_gpu_vgpu.h` (the shared op-table/ioctl header; populated here, extended in Task 4)
- Modify: `kext/virtio_gpu/virtio_gpu_drv_entry.c:148`
- Modify: `Makefile` (`VIRTIO_GPU_OBJS += virtio_gpu_3d.o`)

**Interfaces:**
- Consumes: `struct virtio_device *vdev` (its `->priv` is the `drm_device`, whose `dev_private` is `struct virtio_gpu_device *`, as in `virtio_gpu_present.c`); driver helpers `virtio_gpu_cmd_get_capset_info`, `virtio_gpu_cmd_get_capset`, `virtio_gpu_cmd_context_create`, `virtio_gpu_notify`, `lkpi_wait_pump`; the device's `num_capsets`/`has_virgl_3d` fields on `struct virtio_gpu_device`.
- Produces: `int virtio_gpu_3d_init(struct virtio_device *vdev)` — returns 0 and leaves a created context id in the file-static `g_ctx_id` (>0) when virgl is up, returns -1 (no virgl) otherwise; `int virtio_gpu_3d_available(void)` (1/0); `struct virtio_gpu_device *virtio_gpu_3d_dev(void)`; `uint32_t virtio_gpu_3d_ctx(void)`.

- [ ] **Step 1: Add the virgl feature to the driver's feature table check**

The unmodified driver already lists `VIRTIO_GPU_F_VIRGL` in its `features[]`; confirm `virtio_gpu_drv_entry.c`'s negotiation (the `want |= feature_table[i]` loop already shown at `:124`) carries it. No edit if `virtio_gpu.features[]` includes it (it does in 6.12). Add a log line after `host = vdev->features;`:

```c
knx_log(host & (1ULL << VIRTIO_GPU_F_VIRGL) ? "virtio_gpu: virgl 3D negotiated\n"
                                            : "virtio_gpu: 2D only (no virgl)\n");
```

- [ ] **Step 2: Write `virtio_gpu_3d.c` — capset + context create**

```c
/* virtio_gpu_3d.c — virgl 3D bring-up + the /dev/vgpu op table, over the UNMODIFIED driver. */
#include "virtgpu_drv.h"
#include "virtio_gpu_vgpu.h"
#include "lkpi_knx.h"

extern void lkpi_wait_pump(void);

static struct virtio_gpu_device *g_dev;
static uint32_t g_ctx_id;

int virtio_gpu_3d_available(void) { return g_dev && g_dev->has_virgl_3d; }
struct virtio_gpu_device *virtio_gpu_3d_dev(void) { return g_dev; }
uint32_t virtio_gpu_3d_ctx(void) { return g_ctx_id; }

int virtio_gpu_3d_init(struct virtio_device *vdev)
{
    struct drm_device *ddev = (struct drm_device *)vdev->priv;
    struct virtio_gpu_device *vgdev = ddev ? ddev->dev_private : 0;
    if (!vgdev || !vgdev->has_virgl_3d) {
        knx_log("virtio_gpu: 3d_init skipped (no virgl)\n");
        return -1;
    }
    g_dev = vgdev;
    /* The driver fetched capsets during probe (vgdev->num_capsets); we just need a context. */
    g_ctx_id = 1;
    virtio_gpu_cmd_context_create(vgdev, g_ctx_id, 5, "nanos-nwm");
    virtio_gpu_notify(vgdev);
    lkpi_wait_pump();
    knx_log("virtio_gpu: virgl 3D context up\n");
    knx_vgpu_register(&g_vgpu_ops);   /* defined in Task 3/4 */
    return 0;
}
```

For this task, stub `knx_vgpu_register(&g_vgpu_ops)` out behind `#if 0` (the op table arrives in Task 3); keep the context-create + log.

- [ ] **Step 3: Call it from bringup**

In `virtio_gpu_drv_entry.c` after the `virtio_gpu_fbcon_bringup(vdev)` block (~`:150`):

```c
	if (virtio_gpu_3d_init(vdev) == 0)
		knx_log("virtio_gpu: 3D path ready\n");
```

Add `int virtio_gpu_3d_init(struct virtio_device *vdev);` as a forward decl near the top.

- [ ] **Step 4: Build and verify on both QEMUs**

Run: `make image64`
Then GL QEMU: `SER=/tmp/gl.log DISPLAY_BACKEND=none bash scripts/run64-gl.sh & sleep 40; grep -E "virgl 3D negotiated|virgl 3D context up" /tmp/gl.log; pkill -9 -f image64`
Expected (GL QEMU): both `virgl 3D negotiated` and `virgl 3D context up`.
Then plain QEMU: `make run64` style boot (no `virtio-vga-gl`) — expected: `2D only (no virgl)` and `3d_init skipped`, desktop still renders (2D path intact).

- [ ] **Step 5: Commit**

```bash
git add kext/virtio_gpu/virtio_gpu_3d.c kext/virtio_gpu/virtio_gpu_vgpu.h kext/virtio_gpu/virtio_gpu_drv_entry.c Makefile
git commit -m "virtio_gpu: negotiate virgl + create a 3D context at bringup"
```

---

### Task 3: K2 (part A) — `/dev/vgpu` char device + PROBE/GET_CAPS

Create the kernel `CharDevice` and the kext→kernel op-table registration, with the two read-only ioctls. The ioctl dispatch is pure logic → host-tested.

**Files:**
- Create: `drivers/VgpuDevice.h`, `drivers/VgpuDevice.cpp`
- Modify: `kext/virtio_gpu/virtio_gpu_vgpu.h` (op table + ioctl numbers + arg structs)
- Modify: `kernel/KernelExports.cpp`, `kernel/KernelExports.h`, `linuxkpi/lkpi_knx.h`
- Modify: `kext/virtio_gpu/virtio_gpu_3d.c` (fill `g_vgpu_ops`, enable `knx_vgpu_register`)
- Create: `tests/test_vgpu_ioctl.cpp`; Modify: `Makefile` (TEST_MODULES)

**Interfaces:**
- Produces (`virtio_gpu_vgpu.h`):

```c
#pragma once
#include <stdint.h>
/* ioctl command numbers (NanOS-local, not Linux DRM) */
#define VGPU_PROBE              0x5600u   /* arg: struct vgpu_caps*  */
#define VGPU_RESOURCE_CREATE_3D 0x5601u   /* arg: struct vgpu_res_create* (Task 4) */
#define VGPU_TRANSFER_TO_HOST   0x5602u   /* arg: struct vgpu_transfer*   (Task 4) */
#define VGPU_SUBMIT             0x5603u   /* arg: struct vgpu_submit*     (Task 4) */
#define VGPU_PRESENT            0x5604u   /* arg: struct vgpu_present*    (Task 4) */

struct vgpu_caps { uint32_t available; uint32_t ctx_id; uint32_t max_size; uint32_t reserved; };

struct knx_vgpu_ops {
    int (*probe)(struct vgpu_caps *out);                         /* 0 ok */
    int (*res_create_3d)(const void *arg, uint32_t *handle_out); /* Task 4 */
    int (*transfer_to_host)(const void *arg);                    /* Task 4 */
    int (*submit)(const void *cmdbuf, uint32_t bytes);           /* Task 4 */
    int (*present)(const void *arg);                             /* Task 4 */
};
extern const struct knx_vgpu_ops g_vgpu_ops;
```

- Produces (kernel): `void knx_vgpu_register(const struct knx_vgpu_ops *ops)` stores the table; `VgpuDevice::ioctl` dispatches. `VgpuDevice(const struct knx_vgpu_ops *ops)` ctor.
- Consumes: `CharDevice` (`drivers/CharDevice.h`), `g_root->addChar` (`fs/SynthFs.h`).

- [ ] **Step 1: Write the failing host test for ioctl dispatch**

`tests/test_vgpu_ioctl.cpp`:

```cpp
#include "doctest.h"
#include "VgpuDevice.h"
#include "virtio_gpu_vgpu.h"
using namespace kernel;

static int g_probe_calls = 0;
static int fake_probe(struct vgpu_caps* o){ g_probe_calls++; o->available=1; o->ctx_id=7; o->max_size=4096; return 0; }
static const knx_vgpu_ops FAKE = { fake_probe, 0,0,0,0 };

TEST_CASE("VgpuDevice PROBE forwards to ops and fills caps") {
    VgpuDevice dev(&FAKE);
    vgpu_caps c = {};
    CHECK(dev.ioctl(VGPU_PROBE, &c) == 0);
    CHECK(c.available == 1);
    CHECK(c.ctx_id == 7);
    CHECK(g_probe_calls == 1);
}
TEST_CASE("VgpuDevice rejects unknown ioctl") {
    VgpuDevice dev(&FAKE);
    CHECK(dev.ioctl(0x9999, 0) == -25 /* -ENOTTY */);
}
```

- [ ] **Step 2: Run it; verify it fails to compile (VgpuDevice missing)**

Run: `make test64 2>&1 | grep -i "VgpuDevice\|error" | head`
Expected: FAIL — `VgpuDevice.h: No such file` / undefined `VgpuDevice`.

- [ ] **Step 3: Implement `VgpuDevice`**

`drivers/VgpuDevice.h`:

```cpp
#pragma once
#include "CharDevice.h"
struct knx_vgpu_ops;
namespace kernel {
class VgpuDevice : public CharDevice {
public:
    explicit VgpuDevice(const struct knx_vgpu_ops* ops) : m_ops(ops) {}
    int read(unsigned, void*, unsigned) override { return 0; }
    int write(unsigned, const void*, unsigned) override { return -1; }
    int ioctl(unsigned cmd, void* arg) override;
    int mmapInfo(uint64_t*, unsigned*) override { return -1; }
private:
    const struct knx_vgpu_ops* m_ops;
};
}
```

`drivers/VgpuDevice.cpp`:

```cpp
#include "VgpuDevice.h"
#include "virtio_gpu_vgpu.h"
namespace kernel {
int VgpuDevice::ioctl(unsigned cmd, void* arg) {
    if (!m_ops) return -19; /* -ENODEV */
    switch (cmd) {
    case VGPU_PROBE:
        if (!arg || !m_ops->probe) return -22; /* -EINVAL */
        return m_ops->probe((struct vgpu_caps*)arg);
    /* RESOURCE_CREATE_3D / TRANSFER / SUBMIT / PRESENT — Task 4 */
    default:
        return -25; /* -ENOTTY */
    }
}
}
```

- [ ] **Step 4: Run the test; verify PASS**

Run: `make test64 2>&1 | tail -5`
Expected: PASS (add `VgpuDevice` to `TEST_MODULES`, `tests/test_vgpu_ioctl.cpp` to the suite; the test needs `-Ikext/virtio_gpu` for the header).

- [ ] **Step 5: Wire kernel registration + `/dev/vgpu` node**

In `kernel/KernelExports.h` add: `void knx_vgpu_register(const struct knx_vgpu_ops* ops);` (guard the struct fwd-decl). In `kernel/KernelExports.cpp`:

```cpp
#include "VgpuDevice.h"
#include "virtio_gpu_vgpu.h"
void knx_vgpu_register(const struct knx_vgpu_ops* ops) {
    if (g_root && ops)
        g_root->addChar(g_root->dev(), "vgpu", new kernel::VgpuDevice(ops), 0666);
}
```

In `linuxkpi/lkpi_knx.h` declare `void knx_vgpu_register(const struct knx_vgpu_ops* ops);` (with a matching `struct knx_vgpu_ops;` fwd-decl / include of `virtio_gpu_vgpu.h`).

- [ ] **Step 6: Fill `g_vgpu_ops.probe` in the kext and enable registration**

In `virtio_gpu_3d.c`:

```c
static int vgpu_probe(struct vgpu_caps *o) {
    o->available = virtio_gpu_3d_available();
    o->ctx_id    = g_ctx_id;
    o->max_size  = 4096;
    o->reserved  = 0;
    return 0;
}
const struct knx_vgpu_ops g_vgpu_ops = { vgpu_probe, 0, 0, 0, 0 };
```

Remove the `#if 0` around `knx_vgpu_register(&g_vgpu_ops);` in `virtio_gpu_3d_init`.

- [ ] **Step 7: Verify on GL QEMU that userspace can probe**

Add a one-liner to `gltest` later; for now verify the node exists: boot GL QEMU, log in, `ls -l /dev/vgpu` shows the node. (Serial-scriptable in the smoke gate; manual check acceptable here.)
Expected: `/dev/vgpu` present on GL QEMU.

- [ ] **Step 8: Commit**

```bash
git add drivers/VgpuDevice.* kext/virtio_gpu/virtio_gpu_vgpu.h kernel/KernelExports.* linuxkpi/lkpi_knx.h kext/virtio_gpu/virtio_gpu_3d.c tests/test_vgpu_ioctl.cpp Makefile
git commit -m "vgpu: /dev/vgpu char device + PROBE/GET_CAPS over the virgl 3D context"
```

---

### Task 4: K2 (part B) — resource create / transfer / submit / present ioctls

Wire the four mutating ioctls to the driver's 3D helpers. Verified by a trivial submit that clears the scanout to a solid colour (Task 6's `gltest` exercises a real draw; here a clear proves the submit/fence/present chain).

**Files:**
- Modify: `kext/virtio_gpu/virtio_gpu_vgpu.h` (arg structs), `kext/virtio_gpu/virtio_gpu_3d.c`, `drivers/VgpuDevice.cpp`

**Interfaces:**
- Produces (arg structs in `virtio_gpu_vgpu.h`):

```c
struct vgpu_res_create { uint32_t format; uint32_t width; uint32_t height; uint32_t bind; uint32_t handle_out; };
struct vgpu_transfer   { uint32_t handle; uint32_t width; uint32_t height; uint64_t user_src; uint32_t stride; };
struct vgpu_submit     { uint64_t user_cmdbuf; uint32_t bytes; uint32_t reserved; };
struct vgpu_present    { uint32_t handle; uint32_t width; uint32_t height; };
```

- Consumes: driver helpers `virtio_gpu_cmd_resource_create_3d`, `virtio_gpu_object_create` (backing), `virtio_gpu_cmd_transfer_to_host_3d`, `virtio_gpu_cmd_submit`, `virtio_gpu_cmd_set_scanout`, `virtio_gpu_cmd_resource_flush`, `virtio_gpu_notify`, `lkpi_wait_pump`; a kernel `copy_from_user` equivalent for the `user_*` pointers (`knx_copy_from_user(dst, user_ptr, n)` — add a thin export wrapping the kernel's existing user-copy used by other syscalls).
- Produces (kext): the four `g_vgpu_ops` functions; a small handle→`struct virtio_gpu_object*` table (fixed array of 64 slots).

- [ ] **Step 1: Add `knx_copy_from_user` export (if absent)**

In `kernel/KernelExports.cpp`, export the user-copy the syscall layer already uses:

```cpp
int knx_copy_from_user(void* dst, uint64_t user_src, unsigned n) {
    return kernel::copyFromUser(dst, (const void*)(uintptr_t)user_src, n); /* existing helper */
}
```

Declare in `linuxkpi/lkpi_knx.h`. (Grep `copyFromUser`/`copy_from_user` to use the real existing name.)

- [ ] **Step 2: Implement the four ops in `virtio_gpu_3d.c`**

```c
static struct virtio_gpu_object *g_handles[64];
static uint32_t g_next_handle = 1;

static int vgpu_res_create_3d(const void *arg, uint32_t *handle_out) {
    const struct vgpu_res_create *rc = arg;
    struct virtio_gpu_object_params p = {0};
    struct virtio_gpu_object *bo = 0;
    p.format = rc->format; p.width = rc->width; p.height = rc->height;
    p.target = 2 /* 2D texture */; p.bind = rc->bind; p.dumb = false;
    if (virtio_gpu_object_create(g_dev, &p, &bo, 0) || !bo) return -12; /* -ENOMEM */
    if (g_next_handle >= 64) return -12;
    g_handles[g_next_handle] = bo;
    *handle_out = g_next_handle++;
    return 0;
}
static int vgpu_transfer_to_host(const void *arg) {
    const struct vgpu_transfer *t = arg;
    struct virtio_gpu_object *bo = (t->handle < 64) ? g_handles[t->handle] : 0;
    if (!bo) return -22;
    /* copy user pixels into the bo's contiguous backing, then issue the 3D transfer */
    if (knx_copy_from_user(page_address(bo->base.pages[0]), t->user_src, t->stride * t->height))
        return -14; /* -EFAULT */
    {
        struct virtio_gpu_object_array *objs = virtio_gpu_array_alloc(1);
        if (!objs) return -12;
        virtio_gpu_array_add_obj(objs, &bo->base.base);
        virtio_gpu_cmd_transfer_to_host_3d(g_dev, 0 /* offset */, objs, NULL, 0);
    }
    virtio_gpu_notify(g_dev); lkpi_wait_pump();
    return 0;
}
static int vgpu_submit(const void *cmdbuf, uint32_t bytes) {
    void *buf = kmalloc(bytes, 0);
    if (!buf) return -12;
    if (knx_copy_from_user(buf, (uint64_t)(uintptr_t)cmdbuf, bytes)) { kfree(buf); return -14; }
    virtio_gpu_cmd_submit(g_dev, buf, bytes, g_ctx_id, NULL, NULL);
    virtio_gpu_notify(g_dev); lkpi_wait_pump();
    /* virtio_gpu_cmd_submit takes ownership of/copies buf per driver semantics; if not, free after fence */
    return 0;
}
static int vgpu_present(const void *arg) {
    const struct vgpu_present *pr = arg;
    struct virtio_gpu_object *bo = (pr->handle < 64) ? g_handles[pr->handle] : 0;
    if (!bo) return -22;
    virtio_gpu_cmd_set_scanout(g_dev, 0, bo->hw_res_handle, pr->width, pr->height, 0, 0);
    virtio_gpu_cmd_resource_flush(g_dev, bo->hw_res_handle, 0, 0, pr->width, pr->height, NULL, NULL);
    virtio_gpu_notify(g_dev); lkpi_wait_pump();
    return 0;
}
const struct knx_vgpu_ops g_vgpu_ops = {
    vgpu_probe, vgpu_res_create_3d, vgpu_transfer_to_host, vgpu_submit, vgpu_present
};
```

> NOTE on `virtio_gpu_cmd_submit` ownership and the exact `virtio_gpu_object_params` field names: read `virtgpu_vq.c` / `virtgpu_drv.h` in `external/linux-6.12/.../virtio` and match the real signatures — they are the source of truth. Adjust field names (`target`/`bind`/`format`) to the struct as vendored.

- [ ] **Step 3: Dispatch the four ioctls in `VgpuDevice::ioctl`**

Add cases that copy the small arg struct from `arg` (already a kernel pointer at this layer — the syscall ioctl path passes a kernel-validated pointer; the *large* user buffers are handled inside the ops via `knx_copy_from_user`):

```cpp
    case VGPU_RESOURCE_CREATE_3D: {
        if (!arg || !m_ops->res_create_3d) return -22;
        auto* rc = (struct vgpu_res_create*)arg;
        return m_ops->res_create_3d(rc, &rc->handle_out);
    }
    case VGPU_TRANSFER_TO_HOST: return (arg && m_ops->transfer_to_host) ? m_ops->transfer_to_host(arg) : -22;
    case VGPU_SUBMIT: {
        if (!arg || !m_ops->submit) return -22;
        auto* s = (struct vgpu_submit*)arg;
        return m_ops->submit((const void*)(uintptr_t)s->user_cmdbuf, s->bytes);
    }
    case VGPU_PRESENT: return (arg && m_ops->present) ? m_ops->present(arg) : -22;
```

- [ ] **Step 4: Smoke — clear the scanout via the 3D path**

Temporarily, in `gltest.c` (created in Task 6, or a throwaway here): create a render-target resource, submit a virgl `CLEAR` command buffer (single `VIRGL_CCMD_CLEAR` with a known colour — opcode from `external/virgl/virgl_protocol.h`), present.
Run the GL smoke and `screendump`; expected: the whole screen is the clear colour (proves create→submit→present chain end-to-end).

- [ ] **Step 5: Commit**

```bash
git add kext/virtio_gpu/virtio_gpu_vgpu.h kext/virtio_gpu/virtio_gpu_3d.c drivers/VgpuDevice.cpp kernel/KernelExports.* linuxkpi/lkpi_knx.h
git commit -m "vgpu: resource-create-3d / transfer / submit / present ioctls over the driver helpers"
```

---

### Task 5: U1 (encoder) — host-tested virgl command-buffer builder

The encoder is pure data: it appends dwords in the virgl command format. This is the one piece with real unit tests (byte-exact).

**Files:**
- Create: `external/virgl/virgl_protocol.h`, `external/virgl/virgl_hw.h` (vendored)
- Create: `user/libnwgl/nwgl_encode.h`, `user/libnwgl/nwgl_encode.c`
- Create: `tests/test_nwgl_encode.cpp`; Modify: `Makefile`

**Interfaces:**
- Produces:

```c
/* nwgl_encode.h */
#include <stdint.h>
struct nwgl_cmd { uint32_t *buf; uint32_t len; uint32_t cap; };  /* len/cap in dwords */
void nwgl_cmd_init(struct nwgl_cmd *c, uint32_t *storage, uint32_t cap_dwords);
/* Append a virgl command header: (len<<16)|(obj_type<<8)|cmd, then `ndw` payload dwords from p. */
int  nwgl_emit(struct nwgl_cmd *c, uint8_t cmd, uint8_t obj_type, const uint32_t *p, uint32_t ndw);
uint32_t nwgl_bytes(const struct nwgl_cmd *c);   /* c->len * 4 */
```

- Consumes: virgl opcode/object constants from `external/virgl/virgl_protocol.h`.

- [ ] **Step 1: Vendor the virgl headers**

```bash
mkdir -p external/virgl
cp "$(brew --prefix virglrenderer)/include/virgl/virglrenderer.h" external/virgl/ 2>/dev/null || true
# virgl_protocol.h / virgl_hw.h come from the virglrenderer source tree (src/); fetch the matching tag:
curl -sL https://gitlab.freedesktop.org/virgl/virglrenderer/-/raw/master/src/virgl_protocol.h -o external/virgl/virgl_protocol.h
curl -sL https://gitlab.freedesktop.org/virgl/virglrenderer/-/raw/master/src/virgl_hw.h -o external/virgl/virgl_hw.h
```

- [ ] **Step 2: Write the failing encoder test**

`tests/test_nwgl_encode.cpp`:

```cpp
#include "doctest.h"
extern "C" {
#include "nwgl_encode.h"
}
TEST_CASE("nwgl_emit writes header dword then payload") {
    uint32_t store[8] = {0};
    nwgl_cmd c; nwgl_cmd_init(&c, store, 8);
    uint32_t payload[2] = { 0xAAAA, 0xBBBB };
    CHECK(nwgl_emit(&c, /*cmd*/0x03, /*obj*/0x00, payload, 2) == 0);
    /* header = (len<<16)|(objtype<<8)|cmd, len = payload dwords */
    CHECK(store[0] == ((2u<<16) | (0u<<8) | 0x03u));
    CHECK(store[1] == 0xAAAA);
    CHECK(store[2] == 0xBBBB);
    CHECK(nwgl_bytes(&c) == 3*4);
}
TEST_CASE("nwgl_emit refuses overflow") {
    uint32_t store[2] = {0};
    nwgl_cmd c; nwgl_cmd_init(&c, store, 2);
    uint32_t big[4] = {1,2,3,4};
    CHECK(nwgl_emit(&c, 0x03, 0, big, 4) == -1);
}
```

- [ ] **Step 3: Run it; verify it fails**

Run: `make test64 2>&1 | grep -i "nwgl\|error" | head`
Expected: FAIL — `nwgl_encode.h` not found.

- [ ] **Step 4: Implement the encoder**

`nwgl_encode.c`:

```c
#include "nwgl_encode.h"
void nwgl_cmd_init(struct nwgl_cmd *c, uint32_t *s, uint32_t cap){ c->buf=s; c->len=0; c->cap=cap; }
int nwgl_emit(struct nwgl_cmd *c, uint8_t cmd, uint8_t obj, const uint32_t *p, uint32_t ndw){
    if (c->len + 1 + ndw > c->cap) return -1;
    c->buf[c->len++] = ((uint32_t)ndw << 16) | ((uint32_t)obj << 8) | cmd;
    for (uint32_t i=0;i<ndw;i++) c->buf[c->len++] = p[i];
    return 0;
}
uint32_t nwgl_bytes(const struct nwgl_cmd *c){ return c->len * 4u; }
```

- [ ] **Step 5: Run the test; verify PASS**

Run: `make test64 2>&1 | tail -5`
Expected: PASS. Add `nwgl_encode` to `TEST_MODULES`/`COV_PATTERNS`, `tests/test_nwgl_encode.cpp` to the suite, `-Iuser/libnwgl` to `HINCLUDES`.

- [ ] **Step 6: Commit**

```bash
git add external/virgl user/libnwgl/nwgl_encode.* tests/test_nwgl_encode.cpp Makefile
git commit -m "libnwgl: host-tested virgl command-buffer encoder + vendored virgl protocol headers"
```

---

### Task 6: U1 (shaders + draw) — textured quad on screen via `gltest.nxe`

Bring up the real draw pipeline (vertex buffer, shaders, sampler, draw) end-to-end. The oracle is a `screendump` of a known texture drawn full-screen.

**Files:**
- Create: `user/libnwgl/nwgl.h`, `user/libnwgl/nwgl.c`, `user/libnwgl/shaders.h`
- Create: `user/gltest/gltest.c`
- Modify: `Makefile` (build `libnwgl` + `gltest.nxe`; install to the image)

**Interfaces:**
- Produces (`nwgl.h`):

```c
typedef struct nwgl_ctx nwgl_ctx;
nwgl_ctx *nwgl_open(void);                 /* opens /dev/vgpu; NULL if no virgl */
uint32_t  nwgl_texture(nwgl_ctx*, uint32_t w, uint32_t h);          /* RGBA texture, returns handle */
int       nwgl_upload(nwgl_ctx*, uint32_t tex, const void *pixels, uint32_t stride, uint32_t w, uint32_t h);
uint32_t  nwgl_target(nwgl_ctx*, uint32_t w, uint32_t h);           /* render-target/scanout resource */
int       nwgl_draw_textured_quad(nwgl_ctx*, uint32_t target, uint32_t tex,
                                  int dx, int dy, int dw, int dh);   /* builds + submits a virgl cmdbuf */
int       nwgl_present(nwgl_ctx*, uint32_t target, uint32_t w, uint32_t h);
void      nwgl_close(nwgl_ctx*);
```

- Consumes: `nwgl_encode.*`, `/dev/vgpu` ioctls (`VGPU_*`), the TGSI shaders in `shaders.h`.

- [ ] **Step 1: Author the two shaders in readable TGSI (source of truth)**

In `user/libnwgl/shaders.h`, embed both shaders. The **textured-quad fragment shader** (readable TGSI):

```
FRAG
DCL IN[0], GENERIC[0], PERSPECTIVE      # texcoord
DCL OUT[0], COLOR
DCL SAMP[0]
DCL SVIEW[0], 2D, FLOAT
DCL TEMP[0]
  TEX TEMP[0], IN[0], SAMP[0], 2D
  MOV OUT[0], TEMP[0]
END
```

The **separable Gaussian blur fragment shader** (one pass; direction + radius via a constant; readable TGSI) — a 9-tap kernel sampling `IN[0] ± k*CONST[0].xy`:

```
FRAG
DCL IN[0], GENERIC[0], PERSPECTIVE
DCL OUT[0], COLOR
DCL SAMP[0]
DCL SVIEW[0], 2D, FLOAT
DCL CONST[0]                            # .xy = texel step * direction, .z = unused
DCL TEMP[0], TEMP[1], TEMP[2]
  TEX TEMP[0], IN[0], SAMP[0], 2D
  MUL TEMP[0], TEMP[0], IMM[0].xxxx     # weight 0
  ... (taps ±1..±4 with Gaussian weights, MAD-accumulated) ...
  MOV OUT[0], TEMP[0]
END
```

(Full tap list with weights {0.227,0.194,0.121,0.054,0.016} is written out in the file — no abbreviation in the actual source.) Plus a trivial passthrough **vertex shader** emitting position + texcoord.

- [ ] **Step 2: Tokenize the TGSI offline and embed the tokens**

```bash
# Use Mesa's tgsi text→tokens (or virglrenderer's tgsi parser) to convert each shader to a
# uint32_t token array, then paste the arrays into shaders.h as `static const uint32_t TGSI_FRAG_TEX[] = {...};`
```

Document the exact command used in a comment at the top of `shaders.h` so the tokens are reproducible. (The token arrays are the compiled form virgl's `CREATE_OBJECT(SHADER)` consumes.)

- [ ] **Step 3: Implement `nwgl.c`**

Open `/dev/vgpu`; `nwgl_open` issues `VGPU_PROBE` and returns NULL when `available==0`. `nwgl_texture`/`nwgl_target` issue `VGPU_RESOURCE_CREATE_3D`. `nwgl_upload` issues `VGPU_TRANSFER_TO_HOST`. `nwgl_draw_textured_quad` builds a cmdbuf with `nwgl_encode`: create vertex-elements + a vertex buffer (two triangles with pos+uv), bind the shaders + sampler-view of `tex`, set the framebuffer to `target`, `DRAW_VBO`; then `VGPU_SUBMIT`. `nwgl_present` issues `VGPU_PRESENT`. (Use `virgl_protocol.h` opcodes/object-types.)

- [ ] **Step 4: Write `gltest.c`**

```c
/* gltest.nxe — draw a 64x64 checkerboard texture full-screen via libnwgl. */
#include "nwgl.h"
int main(void){
    nwgl_ctx *g = nwgl_open();
    if(!g){ /* print "no virgl" and exit 1 */ return 1; }
    uint32_t W=1280,H=800;
    uint32_t tgt = nwgl_target(g,W,H);
    uint32_t tex = nwgl_texture(g,64,64);
    static unsigned px[64*64];
    for(int y=0;y<64;y++) for(int x=0;x<64;x++) px[y*64+x] = ((x^y)&8)?0xFFFF00FF:0xFF202020;
    nwgl_upload(g,tex,px,64*4,64,64);
    nwgl_draw_textured_quad(g,tgt,tex,0,0,(int)W,(int)H);
    nwgl_present(g,tgt,W,H);
    /* keep the frame up */ for(;;);
}
```

- [ ] **Step 5: Smoke — checkerboard on screen**

Build `gltest.nxe` into the image; boot GL QEMU; run `gltest` on a VT; `screendump`.
Expected: a magenta/dark checkerboard fills the screen (proves shaders + texture + draw + present through virgl). On plain QEMU `gltest` prints "no virgl" and exits 1 (proves the probe gate).

- [ ] **Step 6: Commit**

```bash
git add user/libnwgl/ user/gltest/ Makefile
git commit -m "libnwgl: TGSI shaders + textured-quad draw path; gltest.nxe bring-up oracle"
```

---

### Task 7: U2 — `nw_compose` GPU backend (wallpaper + windows, no blur yet)

Add the runtime-selected GPU backend that composites the wallpaper and window textures. Blur lands in Task 8. CPU path stays the default when `nwgl_open` returns NULL.

**Files:**
- Modify: `user/nwm/nw_compose.c` (add backend selection + GPU compose path), `Makefile` (link `libnwgl` into `nwm`)

**Interfaces:**
- Consumes: `nwgl.h` (Task 6); the existing `nw_surface` window list + wallpaper buffer in `nw_compose.c`.
- Produces: `static int s_gpu_enabled;` set once at compositor init via `nwgl_open`; a `compose_gpu()` mirroring `compose_cpu()`'s loop but issuing `nwgl_*` calls.

- [ ] **Step 1: Add backend selection at compositor init**

```c
static nwgl_ctx *s_gl;
void nw_compose_init(void){
    s_gl = nwgl_open();           /* NULL when no virgl -> CPU path */
    /* ...existing CPU init... */
}
```

- [ ] **Step 2: Implement `compose_gpu()`**

Upload the wallpaper once to a texture; per frame: for each window, upload its damaged buffer to its texture (`nwgl_upload`), then draw wallpaper quad + each window's textured quad into the scanout target in z-order (`nwgl_draw_textured_quad`), then `nwgl_present`. Route the top-level compose entry:

```c
void nw_compose_frame(void){
    if (s_gl) { if (compose_gpu() == 0) return; /* fall through to CPU on error */ s_gl = 0; }
    compose_cpu();   /* existing path, unchanged */
}
```

(Window content is still software-rendered into each window's buffer — only compositing moves to the GPU.)

- [ ] **Step 3: Smoke — desktop composited on the GPU**

Boot GL QEMU; in the smoke, log in on tty7, let nwm render; `screendump`.
Expected: the desktop (wallpaper + window chrome) renders; serial shows no fault; distinct-colours oracle ≥200 (as in `smoke-virtio-gpu`). Glass windows show *without* blur for now (sharp translucency) — that's expected pre-Task-8.

- [ ] **Step 4: Verify CPU fallback intact**

Boot plain QEMU (no `virtio-vga-gl`): `nwgl_open` returns NULL → `compose_cpu` runs. Desktop renders identically to today.
Expected: `smoke-virtio-gpu` (the existing 2D gate) still PASSES unchanged.

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nw_compose.c Makefile
git commit -m "nwm: GPU compositor backend (wallpaper + window textures) with CPU fallback"
```

---

### Task 8: U2 — backdrop blur as a separable-Gaussian shader

Replace the CPU blur (downsample/cache/budget) with two GPU blur passes on the GPU path. The CPU blur machinery is left in place but unused when `s_gl` is active.

**Files:**
- Modify: `user/nwm/nw_compose.c`, `user/libnwgl/nwgl.c`/`nwgl.h` (add `nwgl_blur`)

**Interfaces:**
- Produces (`nwgl.h`): `int nwgl_blur(nwgl_ctx*, uint32_t src_tex, uint32_t dst_target, int x, int y, int w, int h, float radius);` — two passes (H then V) using the Gaussian shader + a scratch target, each pass a `DRAW_VBO` with `CONST[0]` set to the texel-step × direction.
- Consumes: the Gaussian shader (Task 6 `shaders.h`), a scratch render target sized to the blur region.

- [ ] **Step 1: Implement `nwgl_blur` (two-pass separable)**

Create/reuse a scratch target; pass 1: bind Gaussian shader, `CONST[0] = {1.0/w * radiusStep, 0, ...}`, draw `src_tex` → scratch; pass 2: `CONST[0] = {0, 1.0/h * radiusStep, ...}`, draw scratch → `dst_target` region. Submit one cmdbuf.

- [ ] **Step 2: Use it in `compose_gpu()` for glass windows**

For each `w->glass` window: before drawing the window quad, `nwgl_blur` the backdrop region (the scene already drawn beneath it into the target) into the window rect, then draw the glass quad sampling that blurred region with the rounded-corner+alpha fragment path. The CPU `nw_backdrop_*` cache calls are skipped on the GPU path.

- [ ] **Step 3: Smoke — GPU blur visible**

Boot GL QEMU; `screendump` with a glass window over the wallpaper.
Expected: the backdrop under the glass is blurred (soft gradient, not sharp); distinct-colours oracle ≥200; no fault. Compare against the Task-7 screendump (sharp) to confirm the blur is applied.

- [ ] **Step 4: Verify CPU path still blurs**

Plain QEMU: CPU `nw_compose` still produces its blurred glass (unchanged code path).
Expected: `smoke-virtio-gpu` PASSES; visual blur present on the CPU path.

- [ ] **Step 5: Commit**

```bash
git add user/nwm/nw_compose.c user/libnwgl/nwgl.c user/libnwgl/nwgl.h
git commit -m "nwm: glass backdrop blur as a GPU separable-Gaussian shader (CPU blur unchanged)"
```

---

### Task 9: T2 — `smoke-virtio-gpu-gl` gate + docs

Lock the win behind a CI gate and document the 3D path. The gate skips with a logged notice when no virgl QEMU is available, so default `verify64` on a plain QEMU still passes.

**Files:**
- Create: `scripts/smoke-virtio-gpu-gl.sh`
- Modify: `Makefile` (`smoke-virtio-gpu-gl` target; add to `verify64`), `docs/en/graphics.md`, `docs/pl/graphics.md`, `docs/en/README.md`, `docs/superpowers/specs/2026-06-29-gpu-accel-desktop-virgl-design.md` (status → IMPLEMENTED, phase 1)

**Interfaces:**
- Consumes: `scripts/run64-gl.sh`, the serial markers (`virgl 3D context up`), `gltest`/desktop screendump oracle.
- Produces: `make smoke-virtio-gpu-gl` → PASS/SKIP/FAIL.

- [ ] **Step 1: Write the gate (model on `scripts/smoke-virtio-gpu.sh`)**

Detect GL capability first: `qemu-system-x86_64 -device virtio-vga-gl,help` — if it errors, `echo "SKIP: no virgl-capable QEMU"; exit 0`. Otherwise boot via `run64-gl.sh`, assert serial has `virgl 3D negotiated` + `virgl 3D context up`, log in on tty7, `screendump`, require distinct-colours ≥200 and no `EXCEPTION|PANIC|fault`.

- [ ] **Step 2: Add the Makefile target + wire into verify64**

```make
smoke-virtio-gpu-gl:
	bash scripts/smoke-virtio-gpu-gl.sh
verify64: ... smoke-virtio-gpu-gl
```

- [ ] **Step 3: Run the gate on the GL QEMU**

Run: `make image64 && make smoke-virtio-gpu-gl`
Expected: `PASS: ... GPU-composited desktop with blur via virgl (virtio-vga-gl).`

- [ ] **Step 4: Run full verify64 (incl. the existing 2D smoke + host tests)**

Run: `make verify64`
Expected: all green — host doctests (incl. `nwgl_encode`, `vgpu_ioctl`), the 2D `smoke-virtio-gpu`, and `smoke-virtio-gpu-gl` (or its SKIP on a plain QEMU).

- [ ] **Step 5: Document the 3D path**

In `docs/en/graphics.md` add a "virgl 3D / GPU compositing" section: the `/dev/vgpu` surface, `libnwgl` emitter, the blur shader, runtime fallback, the GL smoke + `run64-gl.sh`; cross-ref `linuxkpi.md`. Mirror in `docs/pl/graphics.md`; add the section to `docs/en/README.md` if needed. Flip the design spec status to **IMPLEMENTED (phase 1)**.

- [ ] **Step 6: Commit**

```bash
git add scripts/smoke-virtio-gpu-gl.sh Makefile docs/
git commit -m "ci+docs: smoke-virtio-gpu-gl gate + document the virgl GPU compositing path"
```

---

## Self-Review

**Spec coverage:**
- API = OpenGL/virgl → Tasks 2–6. ✓
- Compositor-only consumer → Tasks 7–8 (window content stays software-rendered). ✓
- Minimal virgl emitter → Tasks 5–6 (`libnwgl`, no Mesa). ✓
- Bespoke `/dev/vgpu` ioctls (not DRM render-node) → Tasks 3–4. ✓
- Blur = separable-Gaussian shader → Task 8. ✓
- CPU fallback, same binary → Tasks 7 step 4, 8 step 4 (runtime `nwgl_open` gate). ✓
- Drive via unmodified driver helpers → Tasks 2, 4 (all `virtio_gpu_cmd_*`). ✓
- GPL segregation → vendored virgl headers under `external/`, link into kext only (Task 5). ✓
- Host-test the pure-data parts → Tasks 3 (ioctl dispatch), 5 (encoder). ✓
- GL QEMU prerequisite + smoke gate → Tasks 1, 9. ✓
- Phase 2 (Mesa) is explicitly out of this plan (its own spec). ✓

**Known discovery points (not placeholders — flagged for the implementer to resolve against vendored source):**
- Exact `virtio_gpu_object_params` field names and `virtio_gpu_cmd_submit` buffer-ownership semantics → read `external/linux-6.12/.../virtgpu_vq.c` (Task 4 NOTE).
- TGSI token arrays are generated offline from the readable TGSI given in Task 6 step 1; the generating command is recorded in `shaders.h`.
- The real `copyFromUser` symbol name in the kernel (Task 4 step 1).

**Type consistency:** `knx_vgpu_ops` field list, `VGPU_*` ioctl numbers, `vgpu_caps`/`vgpu_res_create`/`vgpu_transfer`/`vgpu_submit`/`vgpu_present` structs, and the `nwgl_*` signatures are used identically across Tasks 3–8. ✓
