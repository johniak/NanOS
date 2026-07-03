# OpenGL Desktop + App GL on QEMU (virgl, real DRM ABI, Mesa) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** NanOS exposes the real Linux DRM ioctl ABI on `/dev/dri/card0` + `/dev/dri/renderD128`, runs ported libdrm + Mesa (Gallium virgl, GL ES 2, EGL, GBM), and the nwm desktop composites on the GPU (incl. the glass blur as a shader) — on QEMU on the macOS M4 host, with the CPU compositor as a permanent runtime fallback.

**Architecture:** The vendored, unmodified `virtio_gpu` driver already compiles its ioctl layer (`virtgpu_ioctl.o`) and the DRM core dispatcher into `virtio_gpu.nkext`. We add: a NanOS `DrmDevice` char device that forwards `SYS_ioctl`/`SYS_mmap` into `drm_ioctl()`/GEM-mmap via a kext-registered op table; an offset-aware device-mmap extension; ports of libdrm and Mesa via the established nx-port flow; and a GL ES backend in `nw_compose` next to the untouched CPU path. Design record: `docs/superpowers/specs/2026-07-01-gpu-stack-gl-vulkan-dell-design.md`.

**Tech Stack:** C (kext glue, nwm), C++ (kernel `CharDevice` glue, doctest), Linux 6.12 vendored DRM (`external/linux-6.12/`), libdrm 2.4.123, Mesa 24.2.x (gallium virgl, EGL, GBM, GLES2 — no LLVM, no GLX), QEMU from the `startergo/homebrew-qemu-virgl-kosmickrisp` tap, docker `nanos-build` image for cross builds.

## Global Constraints

- **x86_64 only.** Build `make image64`; verify `make verify64`. Dev QEMU runs x86_64 TCG on the M4 (`-accel tcg`).
- **The real DRM ioctl ABI, no bespoke devices.** Every userspace-visible ioctl number/struct comes verbatim from `external/linux-6.12/include/uapi/drm/drm.h` + `drm_mode.h` + `virtgpu_drm.h`. No NanOS-invented GPU ioctls.
- **Drive the device only through the unmodified driver + DRM core** — kext glue may call driver/core functions, never re-implement protocol.
- **CPU `nw_compose` path stays fully functional and is the runtime default when GL init fails.** Same `nwm` binary must still boot the desktop on plain QEMU and on the Dell (firmware fb). Any GL error mid-session → log + fall back to CPU for the session, never crash.
- **GPL segregation:** vendored Linux/DRM code links only into `virtio_gpu.nkext`, never `kernel.bin`. Mesa/libdrm (MIT) live in userspace ports.
- **Commits contain NO Claude/AI attribution** — no `Co-Authored-By`, no "Generated with" trailers (hard user rule).
- **Verification model:** pure logic (ioctl dispatch, mmap-offset routing, GL scene translation) is host-tested with doctest via `make test64`; anything touching the device/GPU is verified by a QEMU serial+`screendump` smoke gate (pattern: `scripts/smoke-virtio-gpu.sh`). New GL gates must SKIP (exit 0 with a notice) when the virgl QEMU is absent so plain `verify64` stays green.
- **Existing gates must stay green after every task:** `make verify64` (incl. `smoke-virtio-gpu` 2D path).
- Ports follow the `nxport.toml` manifest flow: `python3 /sdk/port/nanos-port /work/port` inside the **`nanos-sdk-dev:latest`** docker image (the pattern of the openssl/htop/vim ports — see `Makefile:416` `openssl:`). NOTE: `scripts/nx-port-build.sh` + the `nanos-build` image are the OLDER flow (grep/toybox/sudo only) — do not use them for new ports. The `nanos-port` driver lives in the sibling repo `$(NANOS_SDK)` (`~/Projects/nanos-sdk`, `Makefile:263`).

---

## File Structure

**Created:**
- `scripts/run64-gl.sh` — boot `image64` on the virgl QEMU (`virtio-vga-gl`).
- `scripts/smoke-virtio-gpu-gl.sh` — GL smoke gate (3D negotiated, drmtest oracle, GPU desktop).
- `drivers/DrmDevice.h` / `drivers/DrmDevice.cpp` — `/dev/dri/*` char devices; forward ioctl/mmap to the kext op table.
- `kext/virtio_gpu/virtio_gpu_drm_node.c` — kext side: per-process `drm_file`, `drm_ioctl()` forwarding, GEM mmap-offset resolution.
- `kext/virtio_gpu/knx_drm_node.h` — the `knx_drm_ops` table + registration API (shared kext/kernel header, no Linux types).
- `user/drmtest/drmtest.c` — raw DRM-ABI oracle (`GET_CAP`, dumb BO + KMS modeset, virgl EXECBUFFER clear).
- `user/drmtest/virgl_words.h` — ~40 lines of virgl command-stream constants for the clear test (from vendored `virgl_protocol.h`).
- `external/virgl/virgl_protocol.h` — vendored protocol header (reference + drmtest constants).
- `tests/test_drm_node.cpp` — host doctest: DrmDevice dispatch + mmap-offset routing.
- `$(SDK_WORK)/libdrm-port/` — nxport.toml + meson cross file (external, pattern like `$(SDK_WORK)/openssl-port`).
- `$(SDK_WORK)/mesa-port/` — nxport.toml + meson cross file + build hooks.
- `user/gles2info/gles2info.c` — EGL+GLES2 probe (`GL_RENDERER` print) — the Mesa bring-up oracle.
- `user/nwm/nw_compose_gl.c` / `nw_compose_gl.h` — the GL ES compositor backend.
- `docs/superpowers/plans/2026-07-01-plan-1-gl-desktop-virgl-qemu.md` — this plan.

**Modified:**
- `kernel/SyscallDispatch.cpp:878-905` (`SYS_mmap2` device path) — honor the mmap offset for devices.
- `drivers/CharDevice.h` — add default `mmapAt()` (offset-aware variant of `mmapInfo`).
- `kernel/KernelExports.cpp` / `kernel/KernelExports.h` / `kernel/kexports.def` — `knx_drm_register`, `knx_getpid`.
- `linuxkpi/lkpi_knx.h` — declare the new knx imports.
- `kext/virtio_gpu/virtio_gpu_drv_entry.c` — call `virtio_gpu_drm_node_init()` after fbcon bring-up.
- `Makefile` — new objects, `drmtest.nxe`/`gles2info.nxe`, port targets `libdrm`/`mesa`, smoke target, verify64 wiring.
- `user/nwm/nwm.c` — GL backend probe + per-frame present hook.
- `docs/en/graphics.md`, `docs/pl/graphics.md`, `docs/en/linuxkpi.md` — document the DRM nodes + GL stack.

---

### Task 1: virgl-capable QEMU on the M4 + `run64-gl.sh`

Everything downstream needs a QEMU whose `virtio-vga-gl` device exists and renders via virglrenderer→ANGLE→Metal. Homebrew's stock qemu does NOT have it.

**Files:**
- Create: `scripts/run64-gl.sh`

**Interfaces:**
- Produces: `scripts/run64-gl.sh` boots `disk/image64-grub2.img` with `-device virtio-vga-gl`; env `QEMU_GL` overrides the qemu binary; `DISPLAY_BACKEND=none` for headless CI; serial to `$SER` (default `/tmp/nanos-gl.log`). Later tasks and the smoke gate consume exactly this script.

- [ ] **Step 1: Install the GL-enabled QEMU from the tap**

```bash
brew tap startergo/qemu-virgl-kosmickrisp
brew install startergo/qemu-virgl-kosmickrisp/qemu-virgl-kosmickrisp
# Find the installed binary (it is keg-only / suffixed; adjust to what brew prints):
ls "$(brew --prefix qemu-virgl-kosmickrisp)/bin/" | grep qemu-system-x86_64
```

If the tap name/formula differs from the above (taps evolve), consult `brew search qemu-virgl` and prefer any formula providing `qemu-system-x86_64` with virglrenderer; record the exact formula used in the commit message. Fallback tap: `knazarov/homebrew-qemu-virgl`.

- [ ] **Step 2: Confirm the GL device and TCG x86_64 exist**

Run: `"$(brew --prefix qemu-virgl-kosmickrisp)/bin/qemu-system-x86_64" -device virtio-vga-gl,help | head -3`
Expected: a property list including `gl` — NOT `'virtio-vga-gl' not found`.

- [ ] **Step 3: Write the run script**

```bash
#!/usr/bin/env bash
# run64-gl.sh — boot image64 with virgl 3D (virtio-vga-gl + virglrenderer/ANGLE on macOS).
# QEMU_GL: path to a virgl-capable qemu-system-x86_64 (default: the kosmickrisp tap binary).
# DISPLAY_BACKEND: cocoa,gl=es (default, interactive) | none (headless smoke).
set -u
IMG=disk/image64-grub2.img
SER=${SER:-/tmp/nanos-gl.log}
QEMU_GL=${QEMU_GL:-$(brew --prefix qemu-virgl-kosmickrisp 2>/dev/null)/bin/qemu-system-x86_64}
[ -x "$QEMU_GL" ] || { echo "no virgl qemu at $QEMU_GL (set QEMU_GL)"; exit 2; }
[ -f "$IMG" ] || { echo "run 'make image64' first"; exit 2; }
exec "$QEMU_GL" -accel tcg,thread=multi -cpu qemu64 -smp 4 -m 512 \
    -drive file="$IMG",format=raw \
    -device virtio-vga-gl \
    -display "${DISPLAY_BACKEND:-cocoa,gl=es}" \
    -serial file:"$SER" \
    -monitor unix:/tmp/nanos-gl-qmon.sock,server,nowait -no-reboot "$@"
```

Note `gl=es`: on macOS the ANGLE path is the ES profile — matches the tap's docs. If `cocoa,gl=es` errors, try `cocoa,gl=core`.

- [ ] **Step 4: Verify the existing 2D desktop boots on this QEMU**

Run: `make image64 && SER=/tmp/nanos-gl.log DISPLAY_BACKEND=none bash scripts/run64-gl.sh & sleep 60; grep -c "login:" /tmp/nanos-gl.log; pkill -9 -f "qemu-system-x86_64.*image64"`
Expected: `1` or more (boots; driver still on the 2D scanout path). If the driver logs errors about unexpected virgl features, capture the serial log — that is Task 2's input, not a blocker here.

- [ ] **Step 5: Commit**

```bash
git add scripts/run64-gl.sh
git commit -m "build: run64-gl.sh — boot image64 on a virgl-capable QEMU (kosmickrisp tap)"
```

---

### Task 2: virgl negotiation visible + serial markers

The vendored driver negotiates `VIRTIO_GPU_F_VIRGL` itself and sets `vgdev->has_virgl_3d`; it also fetches capsets during probe. This task only *surfaces* that state (serial markers the smoke gates key on) and proves the 2D path is unharmed on both QEMUs.

**Files:**
- Modify: `kext/virtio_gpu/virtio_gpu_drv_entry.c` (after the feature-negotiation block; find it with `grep -n "features" kext/virtio_gpu/virtio_gpu_drv_entry.c`)

**Interfaces:**
- Produces: serial lines `virtio_gpu: virgl 3D negotiated` (GL QEMU) / `virtio_gpu: 2D only (no virgl)` (plain QEMU). Consumed by `smoke-virtio-gpu-gl.sh` (Task 10).

- [ ] **Step 1: Add the marker after feature negotiation**

In `virtio_gpu_drv_entry.c`, after the driver features are accepted (the loop that builds the accepted-feature mask; `VIRTIO_GPU_F_VIRGL` is bit 0 per `include/uapi/linux/virtio_gpu.h`):

```c
    knx_log((vdev->features & (1ULL << VIRTIO_GPU_F_VIRGL))
                ? "virtio_gpu: virgl 3D negotiated\n"
                : "virtio_gpu: 2D only (no virgl)\n");
```

- [ ] **Step 2: Build + check both QEMUs**

Run: `make image64`, then:
1. GL QEMU: `SER=/tmp/gl.log DISPLAY_BACKEND=none bash scripts/run64-gl.sh & sleep 50; grep "virgl 3D negotiated" /tmp/gl.log; pkill -9 -f image64` → expected: the line present.
2. Plain QEMU: `make smoke-virtio-gpu` → expected: PASS (2D gate untouched; serial shows `2D only` only on QEMUs without the GL device — the stock smoke uses plain `virtio-gpu`, so the `2D only` line appears there).

- [ ] **Step 3: Commit**

```bash
git add kext/virtio_gpu/virtio_gpu_drv_entry.c
git commit -m "virtio_gpu: log virgl negotiation state at probe (smoke marker)"
```

---

### Task 3: kernel `DrmDevice` + `knx_drm_register` (host-tested dispatch)

The kernel-side half of the DRM nodes: a `CharDevice` that forwards `ioctl` and (later) mmap-offset queries to a kext-registered op table. Pure dispatch → host-tested. No kext logic yet (Task 4).

**Files:**
- Create: `drivers/DrmDevice.h`, `drivers/DrmDevice.cpp`
- Create: `kext/virtio_gpu/knx_drm_node.h`
- Modify: `kernel/KernelExports.h`, `kernel/KernelExports.cpp`, `kernel/kexports.def`, `linuxkpi/lkpi_knx.h`
- Create: `tests/test_drm_node.cpp`; Modify: `Makefile` (TEST_MODULES + suite)

**Interfaces:**
- Produces (`kext/virtio_gpu/knx_drm_node.h` — plain C, NO Linux headers; included by kext C and kernel C++):

```c
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Node kinds — which DRM node a call came in on. Render nodes only accept the
 * render-allowed subset (DRM core enforces this via drm_file.minor). */
#define KNX_DRM_NODE_PRIMARY 0   /* /dev/dri/card0     */
#define KNX_DRM_NODE_RENDER  1   /* /dev/dri/renderD128 */

struct knx_drm_ops {
    /* Forward one ioctl. pid identifies the calling process (per-process drm_file),
     * node is KNX_DRM_NODE_*, cmd is the FULL Linux ioctl number (incl. size/dir bits),
     * arg is the user pointer as passed to SYS_ioctl. Returns 0/-errno. */
    long (*ioctl)(int pid, int node, unsigned int cmd, void *arg);
    /* Resolve a GEM mmap fake-offset (as returned by the driver in e.g.
     * DRM_IOCTL_VIRTGPU_MAP / mode_create_dumb) to a physical range for this pid.
     * Returns 0 and fills phys/len, or -errno. */
    int  (*mmap_offset)(int pid, uint64_t offset, uint64_t *phys_out, uint64_t *len_out);
    /* Process teardown: release the pid's drm_file + all its GEM handles. */
    void (*release)(int pid);
};

#ifdef __cplusplus
}
#endif
```

- Produces (kernel): `void knx_drm_register(const struct knx_drm_ops *ops)` — creates `/dev/dri/card0` (mode 0666) and `/dev/dri/renderD128` (0666) backed by two `DrmDevice` instances sharing the table. Exported to kexts via `kexports.def`.
- Produces (kernel): `int knx_getpid(void)` — current process id (wraps the scheduler's current-process accessor; find it with `grep -rn "current.*pid\|Process::current" kernel/ | head`). Needed by the kext in Task 4.
- Produces (class): `class DrmDevice : public CharDevice { DrmDevice(const knx_drm_ops*, int node); ... }` with `ioctl(cmd, arg)` → `ops->ioctl(curPid(), m_node, cmd, arg)`; `mmapInfo` returns -1 (DRM mmap is offset-based only — Task 5).
- Consumes: `CharDevice` (`drivers/CharDevice.h`), the `/dev` registration used by `Fb0Device` (find the exact call with `grep -rn "fb0" kernel/ drivers/ | grep -i "addchar\|register" | head -3` and mirror it for a `dri/` subdirectory; if SynthFs has no subdirectory support for /dev, register flat names `dri!card0` → check how `/dev` names with `/` are handled first and, if unsupported, add the two nodes as `/dev/dri` directory + children the same way `/dev` itself is built).

- [ ] **Step 1: Write the failing host test**

`tests/test_drm_node.cpp`:

```cpp
#include "doctest.h"
#include "DrmDevice.h"
#include "knx_drm_node.h"
using namespace kernel;

static int g_calls = 0; static unsigned g_cmd; static int g_node; static int g_pid_seen;
static long fake_ioctl(int pid, int node, unsigned int cmd, void *arg) {
    g_calls++; g_cmd = cmd; g_node = node; g_pid_seen = pid; (void)arg; return 0;
}
static int fake_mmapoff(int, uint64_t off, uint64_t *phys, uint64_t *len) {
    if (off != 0x10000) return -22;
    *phys = 0xABC000; *len = 0x2000; return 0;
}
static const knx_drm_ops FAKE = { fake_ioctl, fake_mmapoff, nullptr };

TEST_CASE("DrmDevice forwards ioctl with node kind and pid") {
    DrmDevice card(&FAKE, KNX_DRM_NODE_PRIMARY);
    DrmDevice rnd (&FAKE, KNX_DRM_NODE_RENDER);
    CHECK(card.ioctl(0xC0106442u /* DRM_IOCTL_VERSION-ish */, nullptr) == 0);
    CHECK(g_node == KNX_DRM_NODE_PRIMARY);
    CHECK(rnd.ioctl(1, nullptr) == 0);
    CHECK(g_node == KNX_DRM_NODE_RENDER);
    CHECK(g_calls == 2);
}
TEST_CASE("DrmDevice with no ops returns -ENODEV") {
    DrmDevice d(nullptr, KNX_DRM_NODE_RENDER);
    CHECK(d.ioctl(1, nullptr) == -19);
}
TEST_CASE("DrmDevice mmapAt resolves via mmap_offset") {
    DrmDevice d(&FAKE, KNX_DRM_NODE_RENDER);
    uint64_t phys = 0; unsigned len = 0;
    CHECK(d.mmapAt(0x10000, &phys, &len) == 0);
    CHECK(phys == 0xABC000); CHECK(len == 0x2000);
    CHECK(d.mmapAt(0x99999, &phys, &len) < 0);
    CHECK(d.mmapInfo(&phys, &len) < 0);   // offset-less mmap not allowed
}
```

(`mmapAt` on CharDevice arrives in Task 5 — for THIS task implement `DrmDevice::mmapAt` as a plain method so the test compiles; Task 5 promotes it to the virtual.)

For the host build, `curPid()` must be stubbable: in `DrmDevice.cpp` guard with `#ifdef HOSTTEST` returning a fixed 42 (mirror how other host-tested kernel files handle it — check `grep -rn "HOSTTEST\|HOST_TEST" tests/ kernel/ drivers/ | head -5` for the repo's exact macro and follow it).

- [ ] **Step 2: Run, verify failure**

Run: `make test64 2>&1 | grep -iE "drm|error" | head`
Expected: FAIL — `DrmDevice.h: No such file or directory`.

- [ ] **Step 3: Implement `DrmDevice`**

`drivers/DrmDevice.h`:

```cpp
/*
 * DrmDevice.h — /dev/dri/card0 + /dev/dri/renderD128. A thin forwarder: the vendored
 * DRM stack lives in virtio_gpu.nkext; the kext registers a knx_drm_ops table and this
 * device routes SYS_ioctl / SYS_mmap(offset) into it. One instance per node kind.
 */
#pragma once
#include "CharDevice.h"
#include <stdint.h>
struct knx_drm_ops;
namespace kernel {
class DrmDevice : public CharDevice {
public:
    DrmDevice(const struct knx_drm_ops* ops, int node) : m_ops(ops), m_node(node) {}
    int read(unsigned, void*, unsigned) override { return -22; }
    int write(unsigned, const void*, unsigned) override { return -22; }
    int ioctl(unsigned cmd, void* arg) override;
    int mmapInfo(uint64_t*, unsigned*) override { return -22; }
    int mmapAt(uint64_t off, uint64_t* physOut, unsigned* lenOut); // virtual in Task 5
private:
    const struct knx_drm_ops* m_ops;
    int m_node;
};
void drmNodesRegister(const struct knx_drm_ops* ops); // called by knx_drm_register
}
```

`drivers/DrmDevice.cpp`:

```cpp
#include "DrmDevice.h"
#include "knx_drm_node.h"
namespace kernel {
#ifdef HOSTTEST
static int curPid() { return 42; }
#else
int syscallCurrentPid(); // provided by the syscall layer; see Step 5
static int curPid() { return syscallCurrentPid(); }
#endif
int DrmDevice::ioctl(unsigned cmd, void* arg) {
    if (!m_ops || !m_ops->ioctl) return -19; /* -ENODEV */
    return (int)m_ops->ioctl(curPid(), m_node, cmd, arg);
}
int DrmDevice::mmapAt(uint64_t off, uint64_t* physOut, unsigned* lenOut) {
    if (!m_ops || !m_ops->mmap_offset) return -19;
    uint64_t len64 = 0;
    int r = m_ops->mmap_offset(curPid(), off, physOut, &len64);
    if (r == 0) *lenOut = (unsigned)len64;
    return r;
}
}
```

- [ ] **Step 4: Run host test, verify PASS**

Run: `make test64 2>&1 | tail -5`
Expected: all PASS. (Wire `DrmDevice` into `TEST_MODULES`; add `-Ikext/virtio_gpu` to the host-test include path for `knx_drm_node.h` — mirror how existing tests include kext headers, `grep -n "Ikext" Makefile`.)

- [ ] **Step 5: Kernel registration + `/dev/dri` nodes + exports**

1. `syscallCurrentPid()`: add to `kernel/Syscall.cpp` a one-liner returning the current process id (use the same accessor `SYS_getpid` uses — `grep -n "SYS_getpid" kernel/SyscallDispatch.cpp` and copy its expression).
2. `knx_getpid` + `knx_drm_register` in `kernel/KernelExports.cpp`:

```cpp
#include "DrmDevice.h"
#include "knx_drm_node.h"
extern "C" int knx_getpid(void) { return kernel::syscallCurrentPid(); }
extern "C" void knx_drm_register(const struct knx_drm_ops* ops) {
    kernel::drmNodesRegister(ops);
}
```

3. `drmNodesRegister` in `DrmDevice.cpp` (kernel build only): create the `dri` directory under `/dev` and add the two char nodes, mirroring exactly how `/dev/fb0` is added (find it: `grep -rn "\"fb0\"" kernel/ drivers/`). Names: `card0`, `renderD128`, mode 0666.
4. Add both symbols to `kernel/kexports.def` and prototypes to `linuxkpi/lkpi_knx.h`:

```c
void knx_drm_register(const struct knx_drm_ops *ops);
int  knx_getpid(void);
```

- [ ] **Step 6: Boot check (nodes exist, inert)**

Run: `make image64 && make run64` (or the qemu-shell script), then in the guest: `ls -l /dev/dri`
Expected: `card0` and `renderD128` listed once Task 4 registers ops. **For this task** the kext does not register yet, so the acceptable result is: kernel builds, boots, no `/dev/dri` yet (registration is kext-driven). The host test is this task's real gate.

- [ ] **Step 7: Commit**

```bash
git add drivers/DrmDevice.* kext/virtio_gpu/knx_drm_node.h kernel/KernelExports.* kernel/kexports.def kernel/Syscall.cpp linuxkpi/lkpi_knx.h tests/test_drm_node.cpp Makefile
git commit -m "drm: /dev/dri node forwarder (DrmDevice) + knx_drm_register/knx_getpid exports"
```

---

### Task 4: kext DRM node backend — per-process `drm_file` → `drm_ioctl()`

The kext side: open a real `drm_file` per calling process against the vendored driver's `drm_device`, and forward ioctls through the DRM core dispatcher so EVERY core + virtgpu ioctl (version, caps, GEM, dumb, KMS, prime, syncobj, virtgpu execbuffer…) works without re-implementation.

**Files:**
- Create: `kext/virtio_gpu/virtio_gpu_drm_node.c`
- Modify: `kext/virtio_gpu/virtio_gpu_drv_entry.c` (call `virtio_gpu_drm_node_init(vdev)` after fbcon bring-up)
- Modify: `Makefile` (`VIRTIO_GPU_OBJS += virtio_gpu_drm_node.o`)

**Interfaces:**
- Consumes: `struct drm_device` (reachable as in `virtio_gpu_present.c` — `vdev->priv`), DRM core `drm_file_alloc()` / `drm_file_free()` (`drm_file.c`, already compiled), `drm_ioctl()` (`drm_ioctl.c` — confirm it's in `DRM_CORE_OBJS`: `grep -n "drm_ioctl.o" Makefile`; if absent, add it and shim link errors the established way), `knx_getpid`.
- Produces: `int virtio_gpu_drm_node_init(struct virtio_device *vdev)` — registers the `knx_drm_ops` table; per-pid table of `struct drm_file*` (fixed 64 slots `{pid, file}` — a DELIBERATE cap, far below the kernel's `ProcTable::MAX = 1024` at `kernel/Process.h:184`; 64 concurrent GPU clients is ample for this system, and the table fails loud (-ENOMEM + `knx_log`) when full).
- Produces: ops implementation used by Task 3's kernel device:

```c
static long node_ioctl(int pid, int node, unsigned int cmd, void *arg);
static int  node_mmap_offset(int pid, uint64_t off, uint64_t *phys, uint64_t *len); /* Task 5 fills */
static void node_release(int pid);
```

- [ ] **Step 1: Study the two dispatch prerequisites (read-only, 15 min)**

1. `external/linux-6.12/drivers/gpu/drm/drm_ioctl.c` — `drm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)`: it takes a **`struct file`** whose `->private_data` is the `drm_file`, and copies the arg struct with `copy_from_user`/`copy_to_user` (both already shimmed in linuxkpi as direct memcpy — user memory is mapped during a syscall).
2. `drm_file_alloc(struct drm_minor *minor)` — needs a `drm_minor`; the vendored `drm_device` has `dev->primary` and `dev->render` minors set up by `drm_dev_init` (verify: `grep -n "render\|primary" external/linux-6.12/drivers/gpu/drm/drm_drv.c | head`). Use `dev->render` for `KNX_DRM_NODE_RENDER`, `dev->primary` otherwise.

Record findings as a comment block at the top of `virtio_gpu_drm_node.c` (signatures + any deviation).

- [ ] **Step 2: Implement the per-pid file table + ioctl forwarding**

`kext/virtio_gpu/virtio_gpu_drm_node.c`:

```c
/*
 * virtio_gpu_drm_node.c — expose the UNMODIFIED driver through the real DRM ioctl ABI.
 * One drm_file per NanOS process (per-open is per-process here: apps open a node once);
 * dispatch through drm_ioctl() so DRM core + virtgpu ioctls all work unmodified.
 */
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <linux/fs.h>
#include "knx_drm_node.h"
#include "lkpi_knx.h"

#define NODE_MAX_CLIENTS 64

static struct drm_device *g_ddev;

struct node_client { int pid; struct drm_file *file; struct file shim; };
static struct node_client g_cli[NODE_MAX_CLIENTS];

static struct node_client *client_get(int pid, int node)
{
    int free_i = -1;
    for (int i = 0; i < NODE_MAX_CLIENTS; i++) {
        if (g_cli[i].file && g_cli[i].pid == pid) return &g_cli[i];
        if (!g_cli[i].file && free_i < 0) free_i = i;
    }
    if (free_i < 0) return 0;
    {
        struct drm_minor *minor = (node == KNX_DRM_NODE_RENDER && g_ddev->render)
                                      ? g_ddev->render : g_ddev->primary;
        struct drm_file *f = drm_file_alloc(minor);
        if (IS_ERR_OR_NULL(f)) return 0;
        g_cli[free_i].pid = pid;
        g_cli[free_i].file = f;
        g_cli[free_i].shim.private_data = f;   /* what drm_ioctl() reads */
        return &g_cli[free_i];
    }
}

static long node_ioctl(int pid, int node, unsigned int cmd, void *arg)
{
    struct node_client *c;
    if (!g_ddev) return -ENODEV;
    c = client_get(pid, node);
    if (!c) return -ENOMEM;
    return drm_ioctl(&c->shim, cmd, (unsigned long)arg);
}

static int node_mmap_offset(int pid, uint64_t off, uint64_t *phys, uint64_t *len)
{
    (void)pid; (void)off; (void)phys; (void)len;
    return -ENOSYS;   /* Task 5 */
}

static void node_release(int pid)
{
    for (int i = 0; i < NODE_MAX_CLIENTS; i++)
        if (g_cli[i].file && g_cli[i].pid == pid) {
            drm_file_free(g_cli[i].file);
            g_cli[i].file = 0; g_cli[i].pid = 0;
        }
}

static const struct knx_drm_ops g_node_ops = { node_ioctl, node_mmap_offset, node_release };

int virtio_gpu_drm_node_init(struct virtio_device *vdev)
{
    struct drm_device *ddev = (struct drm_device *)vdev->priv;
    if (!ddev) return -1;
    g_ddev = ddev;
    knx_drm_register(&g_node_ops);
    knx_log("virtio_gpu: /dev/dri nodes registered\n");
    return 0;
}
```

Expect link errors on first build (`drm_file_alloc`, `drm_ioctl`, missing `struct file` fields…). Resolve them the established LinuxKPI way: add the missing KPI to `linuxkpi/` (real DRM core file lift if it's core code, small shim if it's a leaf), NEVER by editing vendored code. Likely additions: `drm_ioctl.o` to `DRM_CORE_OBJS`; a minimal `struct file` definition in `linuxkpi/include/linux/fs.h` if the current one lacks `private_data`/`f_flags` (check first).

- [ ] **Step 3: Call it at bring-up**

In `virtio_gpu_drv_entry.c`, after the existing fbcon bring-up call (same place the old plan targeted; `grep -n "fbcon_bringup" kext/virtio_gpu/virtio_gpu_drv_entry.c`):

```c
    virtio_gpu_drm_node_init(vdev);
```

(with a forward decl at the top). Register unconditionally — the DRM ABI is useful even on 2D-only QEMU (dumb BOs + KMS); virgl-only ioctls just return errors there.

- [ ] **Step 4: Hook process exit to `release`**

Find where the kernel tears down a process's fds (the `close()` accounting caller — `grep -rn "->close()" kernel/ | head`). `DrmDevice` overrides `close()`: on last close for a pid, call `ops->release(pid)`. Simplest correct version given the current fd layer (no per-fd identity): implement `DrmDevice::close()` to call `m_ops->release(curPid())`. A process that closes one dup'd fd loses its GEM handles early — document this limitation in a comment; nwm/Mesa hold the node open for their lifetime, so it is acceptable for now, and per-fd identity is the recorded follow-on.

- [ ] **Step 5: Boot + first real ioctl from userspace**

Quick manual check before drmtest exists — in the guest shell (the toybox shell can't ioctl; use a 10-line throwaway or jump straight to Task 6's drmtest skeleton): acceptable alternative gate: boot logs show `virtio_gpu: /dev/dri nodes registered` and `ls /dev/dri` lists both nodes on BOTH plain and GL QEMU.

Run: `make image64 && make smoke-virtio-gpu`
Expected: PASS (2D gate green with the node registration in place).

- [ ] **Step 6: Commit**

```bash
git add kext/virtio_gpu/virtio_gpu_drm_node.c kext/virtio_gpu/virtio_gpu_drv_entry.c Makefile linuxkpi/
git commit -m "virtio_gpu: real DRM ioctl ABI on /dev/dri via drm_ioctl() + per-process drm_file"
```

---

### Task 5: offset-aware device mmap (GEM mmap)

Mesa/libdrm map GEM BOs with `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, drm_fd, fake_offset)`. Today the device-mmap path (`kernel/SyscallDispatch.cpp:878` → `Syscalls::mmapInfo`) ignores the offset and maps one fixed region. Add `mmapAt`.

**Files:**
- Modify: `drivers/CharDevice.h` (new virtual), `drivers/DrmDevice.h` (override), `kernel/Syscall.cpp:1009` area, `kernel/SyscallDispatch.cpp:878-905`
- Modify: `tests/test_drm_node.cpp` (already covers mmapAt — promote to virtual, re-run)
- Modify: `kext/virtio_gpu/virtio_gpu_drm_node.c` (implement `node_mmap_offset`)

**Interfaces:**
- Produces (`CharDevice.h`):

```cpp
    // Offset-aware mmap: map the object at device offset `off` (e.g. a GEM fake offset).
    // Default: only offset 0, backed by mmapInfo — existing devices keep working.
    virtual int mmapAt(uint64_t off, uint64_t* physOut, unsigned* lenOut) {
        return off == 0 ? mmapInfo(physOut, lenOut) : -22;
    }
```

- Produces (syscall layer): the `SYS_mmap2` device branch calls `mmapAt(offBytes, …)` instead of `mmapInfo` (`Syscalls::mmapInfo(int fd, …)` gains the off parameter → rename to `mmapAt(int fd, uint64_t off, …)`, update the one call site; keep `vfs->mmapInfo` name changes mechanical and minimal — follow the existing path from `SyscallDispatch.cpp:891`). **Beware**: `SYS_mmap2` offsets are in 4096-byte pages on Linux ABI — check what NanOS's libc-glue passes today (`grep -rn "mmap" user/libc-glue/ | grep -i syscall | head`) and convert consistently; DRM fake offsets are ≥ `DRM_FILE_PAGE_OFFSET_START` (0x100000000 >> 12 pages) so 32-bit page numbers are NOT enough — if the current ABI passes a 32-bit page count, widen the device path to take the raw 64-bit byte offset from the 64-bit syscall register (x86_64 SYS_mmap passes bytes in r9 — verify against `kernel/SyscallDispatch.cpp`'s mmap arg unpacking and fix the width there).
- Produces (kext): `node_mmap_offset` resolves the fake offset via the DRM vma manager:

```c
#include <drm/drm_vma_manager.h>
#include <drm/drm_gem.h>
static int node_mmap_offset(int pid, uint64_t off, uint64_t *phys, uint64_t *len)
{
    struct drm_vma_offset_node *vnode;
    struct drm_gem_object *obj;
    if (!g_ddev) return -ENODEV;
    drm_vma_offset_lock_lookup(g_ddev->vma_offset_manager);
    vnode = drm_vma_offset_exact_lookup_locked(g_ddev->vma_offset_manager,
                                               off >> PAGE_SHIFT, 1);
    drm_vma_offset_unlock_lookup(g_ddev->vma_offset_manager);
    if (!vnode) return -EINVAL;
    obj = container_of(vnode, struct drm_gem_object, vma_node);
    /* linuxkpi shmem backing is one contiguous block: phys of page 0 covers the object.
     * NOTE: drm_gem_shmem_get_pages() is STATIC in 6.12 — the exported entry point is
     * drm_gem_shmem_pin() (drm_gem_shmem_helper.c:262, EXPORT_SYMBOL), which populates
     * shmem->pages under the dma_resv lock and pins them (we never unpin while mapped —
     * matching our no-reclaim KPI). */
    {
        struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
        if (!shmem->pages) {
            int r = drm_gem_shmem_pin(shmem);
            if (r) return r;
        }
        if (!shmem->pages || !shmem->pages[0]) return -ENOMEM;
        *phys = page_to_phys(shmem->pages[0]);
        *len  = obj->size;
    }
    (void)pid;
    return 0;
}
```

Verify the exact `drm_gem_shmem_pin` signature against `external/linux-6.12/drivers/gpu/drm/drm_gem_shmem_helper.c` before use (it is the source of truth). The **contiguity assumption is real** in our KPI (`kpi_mm.c` allocates one block) — assert it: verify `page_to_phys(pages[n]) == page_to_phys(pages[0]) + n*PAGE_SIZE` for the last page and return -EIO with a log if violated (fail loud, not corrupt).

- [ ] **Step 1: Extend the host test (already written in Task 3) — make `mmapAt` virtual, re-run**

Run: `make test64 2>&1 | tail -3` → PASS.

- [ ] **Step 2: Syscall plumbing (width-checked), device branch uses mmapAt**

After editing, run: `make image64 && make smoke-x86_64 && make smoke-vt`
Expected: PASS — /dev/fb0 mmap (offset 0) unchanged via the default `mmapAt`.

- [ ] **Step 3: Implement `node_mmap_offset` (code above), build kext**

Run: `make image64` → links clean (add any missing vma-manager KPI the established way; `drm_vma_manager.o` should already be in `DRM_CORE_OBJS` — verify).

- [ ] **Step 4: End-to-end gate is Task 6's drmtest** (dumb-BO map + pixel write). No standalone gate here beyond green existing smokes.

- [ ] **Step 5: Commit**

```bash
git add drivers/CharDevice.h drivers/DrmDevice.h kernel/Syscall.cpp kernel/SyscallDispatch.cpp kext/virtio_gpu/virtio_gpu_drm_node.c tests/test_drm_node.cpp
git commit -m "mmap: offset-aware device mmap (CharDevice::mmapAt) + GEM fake-offset resolution"
```

---

### Task 6: `drmtest.nxe` — raw DRM-ABI oracle (dumb/KMS + virgl clear)

Before porting anything big, prove the whole kernel surface with raw ioctls: version, caps, dumb BO create/map/write, KMS modeset (SetCrtc), and a virgl-context EXECBUFFER clear. This is the de-risk milestone — if drmtest passes on the GL QEMU, Mesa has everything it needs.

**Files:**
- Create: `user/drmtest/drmtest.c`, `user/drmtest/virgl_words.h`
- Create: `external/virgl/virgl_protocol.h` (vendored: `curl -sL https://gitlab.freedesktop.org/virgl/virglrenderer/-/raw/1.1.1/src/virgl_protocol.h -o external/virgl/virgl_protocol.h` — pin the tag, record it in the file header)
- Modify: `Makefile` (build `drmtest.nxe` like other plain-libc apps — mirror an existing single-file `.nxe` rule, e.g. gltest-style: `$(BINFOLDER)drmtest.nxe: $(DYN_GLUE) $(BINFOLDER)drmtest.o $(BINFOLDER)libc.ndl.a … $(MKNX_TOOL)`; install into image64)

**Interfaces:**
- Consumes: `/dev/dri/card0`, `/dev/dri/renderD128`; UAPI headers straight from the vendored tree: compile with `-I external/linux-6.12/include/uapi -I external/linux-6.12/include` and `#include <drm/drm.h>`, `<drm/drm_mode.h>`, `<drm/virtgpu_drm.h>` (verify these include cleanly with picolibc; where kernel-only types leak, add a tiny `user/drmtest/uapi_compat.h` defining `__user` etc. — keep it under 20 lines).
- Produces: serial markers the GL smoke keys on: `drmtest: version=virtio_gpu`, `drmtest: dumb-map-write OK`, `drmtest: kms-setcrtc OK`, `drmtest: virgl-clear OK` / `drmtest: no-virgl (skip 3D)`; exit 0 on success.

- [ ] **Step 1: Write drmtest part 1 — version + caps + dumb + KMS**

`user/drmtest/drmtest.c` (single file, plain ioctls, no libdrm):

```c
/* drmtest.nxe — raw DRM ABI oracle. Part 1: core+KMS on card0. Part 2: virgl on renderD128. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/virtgpu_drm.h>

static int die(const char *m){ printf("drmtest: FAIL %s\n", m); return 1; }

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) return die("open card0");

    { /* 1. version */
        char name[64] = {0};
        struct drm_version v; memset(&v, 0, sizeof v);
        v.name = name; v.name_len = sizeof name - 1;
        if (ioctl(fd, DRM_IOCTL_VERSION, &v)) return die("VERSION");
        printf("drmtest: version=%s\n", name);          /* expect virtio_gpu */
    }
    struct drm_mode_card_res res; memset(&res, 0, sizeof res);
    uint32_t conns[8], crtcs[8], encs[8], fbs[8];
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return die("GETRESOURCES probe");
    if (res.count_connectors > 8 || res.count_crtcs > 8) return die("too many res");
    res.connector_id_ptr = (uintptr_t)conns; res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;    res.fb_id_ptr = (uintptr_t)fbs;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) return die("GETRESOURCES");

    /* 2. connector + preferred mode */
    struct drm_mode_get_connector c; memset(&c, 0, sizeof c);
    struct drm_mode_modeinfo modes[32]; memset(modes, 0, sizeof modes);
    c.connector_id = conns[0];
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) return die("GETCONNECTOR probe");
    if (c.count_modes > 32) c.count_modes = 32;
    c.modes_ptr = (uintptr_t)modes; c.count_props = 0; c.count_encoders = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) return die("GETCONNECTOR");
    if (!c.count_modes) return die("no modes");
    struct drm_mode_modeinfo *m = &modes[0];
    printf("drmtest: mode %ux%u\n", m->hdisplay, m->vdisplay);

    /* 3. dumb BO + map + fill */
    struct drm_mode_create_dumb cd; memset(&cd, 0, sizeof cd);
    cd.width = m->hdisplay; cd.height = m->vdisplay; cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) return die("CREATE_DUMB");
    struct drm_mode_map_dumb md; memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) return die("MAP_DUMB");
    uint32_t *px = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (px == MAP_FAILED) return die("mmap dumb");
    for (uint64_t i = 0; i < cd.size / 4; i++) px[i] = 0xFF2060C0;  /* solid blue-ish */
    if (px[100] != 0xFF2060C0) return die("readback");
    printf("drmtest: dumb-map-write OK\n");

    /* 4. AddFB + SetCrtc — the screen must turn that colour */
    struct drm_mode_fb_cmd fb; memset(&fb, 0, sizeof fb);
    fb.width = cd.width; fb.height = cd.height; fb.bpp = 32; fb.depth = 24;
    fb.pitch = cd.pitch; fb.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb)) return die("ADDFB");
    struct drm_mode_crtc sc; memset(&sc, 0, sizeof sc);
    sc.crtc_id = crtcs[0]; sc.fb_id = fb.fb_id; sc.set_connectors_ptr = (uintptr_t)conns;
    sc.count_connectors = 1; sc.mode = *m; sc.mode_valid = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) return die("SETCRTC");
    printf("drmtest: kms-setcrtc OK\n");
    sleep(3); /* smoke gate screendumps here */

    return virgl_part(); /* part 2, below */
}
```

- [ ] **Step 2: Write drmtest part 2 — virgl context + EXECBUFFER clear**

Still in `drmtest.c` (with `user/drmtest/virgl_words.h` holding the few protocol constants copied from `external/virgl/virgl_protocol.h`: `VIRGL_CCMD_CLEAR`, `VIRGL_CCMD_CREATE_OBJECT`, `VIRGL_OBJECT_SURFACE`, `VIRGL_CCMD_SET_FRAMEBUFFER_STATE`, command-header macro):

```c
static int virgl_part(void)
{
    int rfd = open("/dev/dri/renderD128", O_RDWR);
    if (rfd < 0) return die("open renderD128");
    struct drm_virtgpu_getparam gp = { .param = VIRTGPU_PARAM_3D_FEATURES, .value = (uintptr_t)&(uint64_t){0} };
    uint64_t has3d = 0; gp.value = (uintptr_t)&has3d;
    if (ioctl(rfd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) || !has3d) {
        printf("drmtest: no-virgl (skip 3D)\n");
        return 0;
    }
    /* 3D resource (render target), bound to a fresh context via ctx handshake:
       the driver creates the drm_file's context lazily on first 3D ioctl in 6.12
       (virtio_gpu_create_context) — EXECBUFFER triggers it. */
    struct drm_virtgpu_resource_create rc; memset(&rc, 0, sizeof rc);
    rc.target = 2; rc.format = 1 /* VIRGL_FORMAT_B8G8R8A8_UNORM */;
    rc.bind = (1 << 1) /* VIRGL_BIND_RENDER_TARGET */;
    rc.width = 640; rc.height = 480; rc.depth = 1; rc.array_size = 1;
    if (ioctl(rfd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc)) return die("RESOURCE_CREATE 3D");
    /* command stream: CREATE_OBJECT(SURFACE of rc.res_handle) + SET_FRAMEBUFFER_STATE + CLEAR */
    uint32_t cmd[64]; unsigned n = 0;
    n = build_clear_stream(cmd, rc.res_handle, 0xFF00FF00u);   /* helper in virgl_words.h */
    struct drm_virtgpu_execbuffer eb; memset(&eb, 0, sizeof eb);
    eb.command = (uintptr_t)cmd; eb.size = n * 4;
    eb.bo_handles = (uintptr_t)&rc.bo_handle; eb.num_bo_handles = 1;
    if (ioctl(rfd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb)) return die("EXECBUFFER clear");
    printf("drmtest: virgl-clear OK\n");
    return 0;
}
```

`build_clear_stream` in `virgl_words.h`: emit `CREATE_OBJECT(SURFACE)` (id 1, res_handle, format, first/last layer 0), `SET_FRAMEBUFFER_STATE` (1 cbuf, surface id 1, no zbuf), `CLEAR` (buffers=PIPE_CLEAR_COLOR0, rgba as 4 floats, depth 1.0, stencil 0) — copy the exact word layouts from `virgl_protocol.h` macros (`VIRGL_OBJ_SURFACE_*`, `VIRGL_SET_FRAMEBUFFER_STATE_*`, `VIRGL_OBJ_CLEAR_*`); ~40 lines total. The oracle is the ioctl chain succeeding + host virglrenderer not erroring (serial/QEMU stderr clean); pixel-level verification of GL output comes with Mesa (Task 8) — do NOT overbuild here.

- [ ] **Step 3: Build + run on plain QEMU (2D subset)**

Run: `make image64`, boot plain QEMU (`make run64` headless or qemu-shell), in guest run `drmtest`.
Expected serial: `version=virtio_gpu`, `dumb-map-write OK`, `kms-setcrtc OK`, `no-virgl (skip 3D)`, exit 0. **The screen shows solid blue during the 3-second hold** (KMS took over scanout from fbcon — this is the money shot for the ABI).
Debugging tips for the implementer: `EINVAL` from GETRESOURCES usually means struct-size mismatch (check `_IOC` size bits vs your struct — you MUST compile against the vendored uapi headers, not host ones); `EFAULT` means the mmapAt width bug (Task 5 step 2).

- [ ] **Step 4: Run on GL QEMU (full)**

Run: `SER=/tmp/gl.log DISPLAY_BACKEND=none bash scripts/run64-gl.sh` + guest `drmtest` (the smoke gate automates this in Task 10 — manual now).
Expected: all four OK markers incl. `virgl-clear OK`.

- [ ] **Step 5: Verify existing gates + commit**

Run: `make verify64` → green (fbcon may need a VT switch to reclaim scanout after drmtest — run drmtest only in smokes/manual, it is not in any boot path).

```bash
git add user/drmtest/ external/virgl/virgl_protocol.h Makefile
git commit -m "drmtest: raw DRM ABI oracle — dumb/KMS modeset + virgl execbuffer clear"
```

---

### Task 7: libdrm port

Mesa consumes libdrm (`drmIoctl`, `drmGetVersion`, `drmMode*`, `drmPrime*`). Port it via the established external-port flow (like openssl-port/htop-port: an `nxport.toml` + hooks under `$(SDK_WORK)`).

**Files:**
- Create (external): `$(SDK_WORK)/libdrm-port/nxport.toml`, `$(SDK_WORK)/libdrm-port/cross-nanos.ini`, `$(SDK_WORK)/libdrm-port/hooks/pre_configure.sh`
- Modify: `Makefile` (target `libdrm`, pattern-copy the `openssl:` target at `Makefile:416`)

**Interfaces:**
- Consumes: the `nanos-sdk-dev:latest` docker image + the `nanos-port` driver from the sibling repo `~/Projects/nanos-sdk`.
- Produces: `libdrm.a` + headers installed into the port sysroot the same place openssl installs its libs (inspect `$(SDK_WORK)/openssl-port/nxport.toml` for the exact `prefix`/destination and mirror it). Mesa (Task 8) consumes exactly these.

- [ ] **Step 0: VERIFY THE PORT FLOW SUPPORTS MESON (blocking precondition)**

The `nanos-port` driver lives in `~/Projects/nanos-sdk` (`port/nanos-port`), NOT in this repo, and no existing port uses meson (openssl = custom Configure, htop/vim = autotools). Before anything else:

```bash
grep -n "meson" ~/Projects/nanos-sdk/port/nanos-port || echo "NO MESON SUPPORT"
docker run --rm nanos-sdk-dev:latest sh -c 'command -v meson ninja || echo MISSING'
```

If meson is unsupported/missing: EITHER extend `nanos-port` + the `nanos-sdk-dev` image in the sibling repo first (preferred — keeps one flow; commit there per that repo's conventions), OR drive meson directly from a `hooks/` shell script inside the container (acceptable; document the deviation in the port's nxport.toml comment and in this plan's commit message). Do not silently improvise mid-task.

- [ ] **Step 1: Fetch + pin libdrm**

```bash
cd "$SDK_WORK" && curl -sLO https://dri.freedesktop.org/libdrm/libdrm-2.4.123.tar.xz && tar xf libdrm-2.4.123.tar.xz
mkdir -p libdrm-port/hooks
```

- [ ] **Step 2: Write the meson cross file**

`$(SDK_WORK)/libdrm-port/cross-nanos.ini` — the toolchain lives at `$(SDK_TC) = $(SDK_WORK)/toolchain` (see `Makefile:149-152, 264`), triple `x86_64-nanos`; verify the exact in-container mount path by inspecting how the openssl port's hooks reference the compiler, then use that spelling:

```ini
[binaries]
c = '<SDK_TC>/bin/x86_64-nanos-gcc'
ar = '<SDK_TC>/bin/x86_64-nanos-ar'
strip = '<SDK_TC>/bin/x86_64-nanos-strip'
pkg-config = 'false'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[built-in options]
default_library = 'static'
```

(`system = 'linux'` on purpose: NanOS speaks the Linux syscall/ioctl ABI and picolibc+libc-glue provide the headers; fixing meson's OS probing is not worth a custom platform. Where a Linux-ism breaks, patch it in `pre_configure.sh` — honest-conftest rule from the SDK memory: never fake a configure answer, fix the gap.)

- [ ] **Step 3: Configure minimal + build**

`hooks/pre_configure.sh` runs (inside docker):

```bash
meson setup build src --cross-file /work/port/cross-nanos.ini \
  -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
  -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled \
  -Dman-pages=disabled -Dtests=false -Dcairo-tests=disabled -Dvalgrind=disabled
ninja -C build
```

Expected first-run failures and their fixes (do them, don't fake them): missing `sys/sysmacros.h` / `major()`/`minor()` → provide in libc-glue; `readdir`/`/dev/dri` node discovery in `xf86drm.c` uses `%s/card%d` paths — works with our `/dev/dri` layout; `drmGetDevices2` may need `stat().st_rdev` majors — NanOS `st_ino=1` era quirks live in libc-glue, extend there if needed.
Expected: `build/libdrm.a` exists; `nm build/libdrm.a | grep drmIoctl` shows the symbol.

- [ ] **Step 4: Install into the port sysroot + Makefile target**

Mirror the `openssl:` Makefile target (`Makefile:416`) as `libdrm:` → runs the port build in docker, copies `libdrm.a` + `include/` into the same sysroot location Mesa will consume. Document the exact destination in the target's comment.

- [ ] **Step 5: Link sanity — rebuild drmtest against libdrm (optional but cheap oracle)**

Add a `drmtest` variant compile using `drmIoctl()` instead of raw `ioctl` for ONE call (version) to prove the .a links into an .nxe. Keep drmtest itself raw (it is the no-dependencies oracle).
Run: guest `drmtest` still all-OK.

- [ ] **Step 6: Commit (repo side) + snapshot the port dir**

```bash
git add Makefile docs/
git commit -m "ports: libdrm 2.4.123 cross-build target (meson, static, virtgpu-only)"
# $(SDK_WORK)/libdrm-port is external — snapshot per the SDK-work convention
# (check how openssl-port is versioned there and follow it).
```

---

### Task 8: Mesa port (gallium virgl + EGL surfaceless + GLES2) + `gles2info`

The big port. Meson cross build of Mesa 24.2.x with the smallest useful feature matrix; the linked artifact is a single static `libEGL+GLESv2+gallium-virgl` archive that apps (and nwm) link; validated by `gles2info.nxe` printing the virgl renderer string.

**Files:**
- Create (external): `$(SDK_WORK)/mesa-port/nxport.toml`, `cross-nanos.ini` (copy from libdrm-port), `hooks/pre_configure.sh`, `hooks/notes.md` (living log of every gap fixed — the next port reads it)
- Create: `user/gles2info/gles2info.c`
- Modify: `Makefile` (target `mesa`, build+install `gles2info.nxe`, link vars `MESA_LIBS`)

**Interfaces:**
- Consumes: libdrm sysroot (Task 7), pthread port, picolibc+libc-glue.
- Produces: static libs (`libEGL_mesa.a`-equivalent set — meson `-Ddefault_library=static` produces `libEGL.a libGLESv2.a` plus internal `.a`s; capture the final link closure into `MESA_LIBS` in the Makefile) + headers (`EGL/egl.h`, `GLES2/gl2.h`).
- Produces: `gles2info.nxe`: EGL_PLATFORM_SURFACELESS init → prints `GL_RENDERER=virgl (…)`; exit 0. Serial marker `gles2info: renderer=` consumed by the smoke.

- [ ] **Step 1: Fetch + pin Mesa**

```bash
cd "$SDK_WORK" && curl -sLO https://archive.mesa3d.org/mesa-24.2.8.tar.xz && tar xf mesa-24.2.8.tar.xz
mkdir -p mesa-port/hooks
```

- [ ] **Step 2: Configure — the minimal matrix**

`hooks/pre_configure.sh`:

```bash
meson setup build src --cross-file /work/port/cross-nanos.ini \
  -Dplatforms= -Degl-native-platform=surfaceless \
  -Dgallium-drivers=virgl -Dvulkan-drivers= \
  -Dglx=disabled -Dgbm=enabled -Degl=enabled \
  -Dgles1=disabled -Dgles2=enabled -Dopengl=false \
  -Dllvm=disabled -Dshared-glapi=disabled \
  -Dshader-cache=disabled -Dzstd=disabled -Dexpat=disabled \
  -Dxmlconfig=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dbuild-tests=false -Dvideo-codecs= \
  -Ddefault_library=static --buildtype=debugoptimized
ninja -C build
```

Notes for the implementer:
- Options drift across Mesa versions. On any "unknown option", run `meson configure build | grep <name>` and use the 24.2 spelling; document each change in `hooks/notes.md`.
- Build-host deps inside docker: python3-mako, bison, flex (glslang not needed — no vulkan). Extend the `nanos-sdk-dev` image (sibling repo) if missing.
- **Expected porting gaps** (fix in libc-glue or a mesa-port patch, never fake): `mmap64/ftruncate` on shm paths (surfaceless+virgl needs none of X/wayland); `pthread_setname_np` (stub in libc-glue); `getenv("MESA_*")` fine; `dlopen` — with static gallium + `shared-glapi=disabled` Mesa's loader short-circuits (megadriver static); if a stray `dlopen` remains in `src/loader`, patch the loader to return the static entrypoint (documented patch file in mesa-port/patches/, applied by pre_configure — keep patches minimal and upstream-shaped).
- The virgl winsys speaks `DRM_IOCTL_VIRTGPU_*` via libdrm on `/dev/dri/renderD128` — exactly what Tasks 4-6 proved.

Expected: ninja completes; `build/src/egl/libEGL.a` (or the static equivalents) exist.

- [ ] **Step 3: `gles2info.c`**

```c
/* gles2info — Mesa bring-up oracle: surfaceless EGL context + renderer string. */
#include <stdio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

int main(void)
{
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, 0);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, 0, 0)) { printf("gles2info: no-display\n"); return 1; }
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                       EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(d, cfg_attr, &cfg, 1, &n) || !n) { printf("gles2info: no-config\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c))
        { printf("gles2info: no-context\n"); return 1; }
    printf("gles2info: renderer=%s\n", (const char *)glGetString(GL_RENDERER));
    printf("gles2info: version=%s\n",  (const char *)glGetString(GL_VERSION));
    /* FBO render + readback — the pixel-true oracle Mesa gives us that drmtest could not: */
    GLuint fbo, rb; glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4 /* ES2-core */, 64, 64);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        { printf("gles2info: fbo-incomplete\n"); return 1; }
    glClearColor(1.f, 0.f, 1.f, 1.f); glClear(GL_COLOR_BUFFER_BIT); glFinish();
    unsigned char px[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf(px[0] > 200 && px[1] < 50 && px[2] > 200 ? "gles2info: clear-readback OK\n"
                                                    : "gles2info: clear-readback BAD\n");
    return !(px[0] > 200);
}
```

- [ ] **Step 4: Link `gles2info.nxe`**

Makefile: `MESA_LIBS := <the .a closure from the mesa build> libdrm.a`; link rule mirrors other libc-linking apps. First link WILL surface missing libc symbols (the htop port needed realpath/getpriority-class stubs) — add each to libc-glue with a real implementation or an honest ENOSYS stub, logged in `mesa-port/hooks/notes.md`.

- [ ] **Step 5: Run on GL QEMU**

Guest: `gles2info`
Expected serial: `renderer=virgl…`, `clear-readback OK`. On plain QEMU: `no-display` or renderer=softpipe is NOT expected (softpipe disabled) → `no-display`/`no-config`, exit 1 — fine.
This is the milestone: **unmodified Mesa running NanOS-side, GPU-backed.**

- [ ] **Step 6: Commit**

```bash
git add user/gles2info/ Makefile docs/
git commit -m "ports: Mesa 24.2 gallium-virgl (EGL surfaceless + GLES2, static) + gles2info oracle"
```

---

### Task 9: GBM + KMS present path (`eglSwapBuffers` → screen)

Offscreen GL works; now put GL frames on the display the canonical way: GBM surface on `card0`, EGL GBM platform, `drmModeAddFB2` + `drmModeSetCrtc`/page-flip. This exact code path carries unchanged to i915 on the Dell.

**Files:**
- Modify (external): `$(SDK_WORK)/mesa-port/hooks/pre_configure.sh` — add `gbm` platform: `-Dplatforms=` stays, GBM is separate (`-Dgbm=enabled` already set); EGL GBM platform needs `-Degl-native-platform=drm`? — No: keep surfaceless default, request GBM at runtime via `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm_device, …)`. Rebuild.
- Create: `user/glkms/glkms.c` — present oracle: GL-render a color gradient + swap to the panel via KMS.
- Modify: `Makefile` (glkms.nxe)

**Interfaces:**
- Consumes: `gbm.h` (from the Mesa build), libdrm KMS calls, `/dev/dri/card0`.
- Produces: `glkms` serial markers `glkms: flip OK` + a visible gradient; the exact init sequence (~120 lines) that `nw_compose_gl.c` (Task 10) reuses verbatim — keep it in a small shared file `user/glkms/glkms_init.c` + `glkms_init.h` with:

```c
struct glkms { int fd; struct gbm_device *gbm; struct gbm_surface *surf;
               EGLDisplay dpy; EGLContext ctx; EGLSurface esurf;
               uint32_t crtc_id, conn_id; struct drm_mode_modeinfo mode;
               struct gbm_bo *front; uint32_t front_fb; };
int  glkms_open(struct glkms *g);            /* full init; 0 ok */
int  glkms_swap(struct glkms *g);            /* eglSwapBuffers + lock front + AddFB2 + SetCrtc/flip */
void glkms_close(struct glkms *g);
```

- [ ] **Step 1: Implement `glkms_init.c`**

Sequence (each step's failure → cleanup + -1, caller falls back):
1. `open("/dev/dri/card0", O_RDWR)`; 2. `gbm_create_device(fd)`; 3. KMS discovery (reuse drmtest's GETRESOURCES/GETCONNECTOR raw code or libdrm `drmModeGetResources`); 4. `gbm_surface_create(gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)`; 5. `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm, 0)` + init + config (`EGL_NATIVE_VISUAL_ID == GBM_FORMAT_XRGB8888`) + window surface on the gbm_surface + context + makeCurrent.
`glkms_swap`: `eglSwapBuffers` → `gbm_surface_lock_front_buffer` → `drmModeAddFB2(fd, w, h, XRGB8888, {gbm_bo_get_handle(bo).u32,0,0,0}, {stride,0,0,0}, {0}, &fb, 0)` → first swap `drmModeSetCrtc`, later swaps `drmModePageFlip` + wait for the flip event via `drmHandleEvent` (needs `read` on the drm fd delivering DRM events — **check**: does our DrmDevice `read()` return -22 (Task 3)? DRM events come via `read()` on Linux. First version: use blocking SetCrtc every swap (no flip events needed — tearing acceptable at bring-up), and record page-flip events as the follow-on requiring `DrmDevice::read` → `drm_read()` forwarding + `pollReady` → `drm_poll`; note it in the task-10 gate). Release the previous front bo (`gbm_surface_release_buffer`) after the new one is on screen.

- [ ] **Step 2: `glkms.c` — gradient oracle**

GL: fullscreen quad, `gl_FragColor = vec4(uv.x, uv.y, 0.5, 1.0)` (one tiny shader pair compiled inline), 60 swaps in a loop, print `glkms: flip OK` after the first successful swap, hold 3 s, restore nothing (fbcon reclaim on VT switch — same caveat as drmtest).

- [ ] **Step 3: Run on GL QEMU + screendump**

Guest: `glkms` via the qemu-shell; then `screendump` through the monitor socket (mirror `scripts/smoke-virtio-gpu.sh`'s screendump helper).
Expected: serial `glkms: flip OK`; screendump shows a two-axis gradient (distinct-colours oracle ≥ 1000 — far above any console screen).

- [ ] **Step 4: Verify plain QEMU fallback + existing gates**

Plain QEMU: `glkms` prints an open/EGL failure and exits nonzero without touching the display. `make verify64` green.

- [ ] **Step 5: Commit**

```bash
git add user/glkms/ Makefile
git commit -m "glkms: GBM+EGL+KMS present path — GL frames on the scanout via AddFB2/SetCrtc"
```

> **STATUS (2026-07-03): DONE — full GL pipeline works over virgl. glkms renders its gradient from a
> real vertex buffer; host 3D-scanout SOLVED; the guest→host buffer-upload bug is FIXED at root cause
> (32-bit mmap-offset truncation in libc-glue, commit de98534).**
>
> **ROOT-CAUSE FIX (de98534):** the libc-glue `mmap` wrapper passed the offset through
> `sys5(int,…,int e)`, casting `off_t` to `int`. DRM GEM fake mmap offsets are ≥ `0x100000000` (the vma
> manager allocates from `DRM_FILE_PAGE_OFFSET_START` = `1<<20` pages), so the high bits were stripped —
> `0x1003e8000` reached the kernel as `0x3e8000`. `drm_vma_offset_exact_lookup` then missed (page 1000 vs
> 1049576), `SYS_mmap` silently fell back to **anonymous zero pages**, and every Mesa vertex/index/uniform
> upload went to throwaway memory while the resource backing the host reads over virgl stayed zero.
> Buffer-sourced draws collapsed to a degenerate primitive → rendered nothing (which had masqueraded as a
> "Mesa draw no-op"). Fix: a dedicated `sys_mmap` passes the offset 64-bit-wide (x86_64 r8). Bisected with
> kernel `node_mmap_offset` logging (`vnode=MISS` → `vnode=OK` after the fix) + a virglrenderer
> `transfer_write_iov` byte dump (host now reads `00 00 80 bf 00 00 80 bf 00 00 40 40` = -1,-1,3).
> `glkms` uses an ES3 `in vec2 p` shader sourcing a real `GL_ARRAY_BUFFER`; the gradient fills the window.
> **Task 10 (nwm GL) is unblocked** — arbitrary vertex-buffer geometry now uploads correctly.
>
> --- earlier findings (host fixes; still valid) ---
> The earlier "host-fork limitation" conclusion (2026-07-02) is **partly overturned**. Built the fork
> from source (QEMU v10.1.0 + the tap's texture-borrowing patch + virglrenderer 1.3.0, all in
> `$(SDK_WORK)/qemu-fork-build` + `virgl-fork-build`) and found the host non-present was **three real
> macOS-cocoa-GL bugs**, now fixed:
> - **GAP 1 — QEMU never set `VIRGL_RENDERER_NATIVE_SHARE_TEXTURE` on macOS** (only under the Windows/
>   D3D11 branch), so virglrenderer never gave scanout resources a Metal texture. Fixed: macOS branch in
>   `virtio_gpu_virgl_init` (`hw/display/virtio-gpu-virgl.c`).
> - **GAP 2 — console mismatch, and it needs NO source patch.** The default `-vga std` device creates
>   graphic console idx 0 *alongside* `virtio-gpu-gl-pci` (idx 1); cocoa binds idx 0, the GPU scans out
>   on idx 1, so `dpy_gl_scanout_texture` is dropped (`con != dcl->con`). **Fix = add `-vga none`** to the
>   QEMU command line — the GPU becomes idx 0, cocoa binds it, the unmodified dispatch path fires. (The
>   earlier `NANOS_RELAX_CON` console.c hack is retired.) Landed in `QEMU_GL_VGA` (Makefile).
> - **GAP 3 — cocoa bound a foreign GL id** from virglrenderer's non-shared context (black). Fixed:
>   cocoa imports the resource's Metal texture (`d3d_tex2d`) as an EGLImage via
>   `EGL_METAL_TEXTURE_ANGLE` and samples that (`ui/cocoa.m`).
> With those three, **`glpix` displays full magenta on the cocoa window** — proven with an *isolated*
> `screencapture` of only the QEMU window rectangle (corners all `(234,51,247)`; earlier full-screen
> captures were contaminated by the host desktop and are not a valid oracle). So the host **does**
> present a virgl 3D-rendered resource as KMS scanout. Fixes saved in
> `$(SDK_WORK)/qemu-fork-build/nanos-fixes/*.modified`.
>
> **PIPELINE PROVEN — `glkms` now DISPLAYS a real GL-rendered gradient (2026-07-03).** The earlier
> "Mesa `glDrawArrays` is a silent no-op" framing is **overturned**: the draw runs and presents. Switching
> `glkms` to an **attribute-less ES3 draw** — a fullscreen triangle generated from `gl_VertexID`, so **no
> vertex buffer is bound** — makes the two-axis fragment-shader gradient fill the whole QEMU window
> (isolated-window corners `BL≈(12,14,123)`, `BR≈(222,52,127)`, `TR≈(243,247,144)`; the FS
> `vec4(v_uv.x,v_uv.y,0.5,1)` clearly ran per-pixel). So vertex-shader → rasteriser → fragment-shader →
> gbm-scanout → Metal → cocoa is **fully functional** end-to-end over virgl on NanOS.
>
> **The one remaining bug is precisely isolated: the guest→host VERTEX-BUFFER data upload lands as zeros.**
> Instrumenting virglrenderer proved it: for a 24-byte `glBufferData` VBO the transfer **does** reach the
> host (`vrend_renderer_transfer_write_iov`, `target=GL_ARRAY_BUFFER`, `w=24`, one iov, host addr set), but
> the bytes read back from the guest iov are **all zero** (`[BUFDBG]`/`[TWDBG]` both read `0.00×6` where
> `{-1,-1,3,-1,-1,3}` was written). Result: all three vertices collapse to `(0,0)` → a degenerate triangle →
> the buffer-sourced draw produces no fragments *even in the guest's own `glReadPixels`*, while the
> host draw call itself is error-free (`fbstatus=COMPLETE`, `vp=[0,0,1280,800]`, no scissor/cull/depth,
> pre/post `GLerr=0`). Neither client-side arrays nor an explicit `glBufferData` VBO changed it (same zeros),
> and it is **not** the ANGLE map-read: the zeros are seen at the transfer's guest-iov source, before any
> ANGLE buffer storage. **Root cause is guest-side buffer-resource backing** (LinuxKPI virtio_gpu GEM/shmem
> allocation + `RESOURCE_ATTACH_BACKING`, or a Mesa buffer map returning a bounce page): the page the host
> reads via the resource iov is not the page the guest CPU wrote its vertex data to. This is the focused
> next fix and is tractable without touching the host. (The kernel fb0-mirror texture path is what makes the
> 2D desktop display today; buffer resources take a different backing path that reads zero.)
>
> **Build-flow gotcha (cost real cycles):** `make image64` does **not** rebuild `glkms` — it only copies
> the existing `bin/glkms.nxe` into the image. Editing `user/glkms/*.c` requires `make glkms` FIRST (Docker
> + `mesa-port/build-glkms.sh`), then `make image64` to package. (Same for `gles2info`/`mesa`.) Also: the
> kernel fb0 mirror floods the host with 1280×800 texture transfers from boot — any bounded host-side
> transfer log must exclude the big texture or the buffer transfer never fits in the budget.
>
> **Valid display oracle:** an *isolated* macOS `screencapture` of just the QEMU window rectangle
> (AXPosition/AXSize → `screencapture -R`), analysed for the expected colour. Full-screen captures and the
> gl=es monitor `screendump` are both invalid (desktop contamination / cannot read the ANGLE-Metal scanout).
> **Task 10 (nwm GL) is unblocked in principle** (the pipeline works) but needs the guest buffer-upload fix,
> because nwm composites arbitrary geometry from vertex buffers (not attribute-less fullscreen triangles).

---

### Task 10: nwm GL backend (`nw_compose_gl.c`) + GL smoke gate

The payoff: nwm composites via GL ES (wallpaper, window textures, glass with GPU two-pass Gaussian blur), runtime-selected, CPU path untouched; locked in by `smoke-virtio-gpu-gl`.

**Files:**
- Create: `user/nwm/nw_compose_gl.c`, `user/nwm/nw_compose_gl.h`
- Modify: `user/nwm/nwm.c` (probe + per-frame hook), `Makefile` (nwm links MESA_LIBS + glkms_init.o), `scripts/smoke-virtio-gpu-gl.sh` (create), `Makefile` verify64 wiring
- Modify: `docs/en/graphics.md`, `docs/pl/graphics.md`, `docs/en/linuxkpi.md`

**Interfaces:**
- Consumes: `struct nw_server` window list + the scene inputs of `nw_compose_scene` (`user/nwm/nw_compose.h:41`), `glkms_init.h`, GLES2.
- Produces (`nw_compose_gl.h`):

```c
/* GL ES compositor backend. All functions return 0 on success; any failure after init
 * disables the backend for the session (nwm falls back to nw_compose_scene). */
int  nw_gl_init(int screen_w, int screen_h);          /* glkms_open + shader compile */
int  nw_gl_frame(const struct nw_server *s,
                 const struct nw_surface *wall);      /* full composite + swap */
void nw_gl_window_dirty(int win_idx);                 /* invalidate that window's texture */
void nw_gl_shutdown(void);
int  nw_gl_active(void);
```

- Behavior contract: visual parity with the CPU path (same z-order, rounded corners, per-window alpha, focus highlight, cursor drawn by nwm.c's overlay logic — on the GL path the cursor is a small textured quad drawn last each frame, since there is no cheap CPU overlay on a GPU-swapped buffer); blur = 2-pass separable Gaussian (radius from `NW_BD_BLUR_RADIUS`, weights {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216}) into a half-res offscreen FBO, sampled by the glass shader; the CPU blur cache/budget machinery (`nw_backdrop.c`) is simply not called on the GL path.

- [ ] **Step 1: Shaders + resources in `nw_compose_gl.c`**

Four programs (compiled at `nw_gl_init`, any compile/link failure → log the info-log + return -1). All share one vertex shader; geometry is always a unit quad instanced per draw via a rect uniform — no vertex buffers per window:

```glsl
/* VS_QUAD — shared. a_pos in [0,1]^2 (one static VBO of 2 triangles). */
attribute vec2 a_pos;
uniform vec4 u_rect;      /* x,y,w,h in pixels */
uniform vec2 u_screen;    /* screen w,h in pixels */
varying vec2 v_uv;
void main() {
    v_uv = a_pos;
    vec2 px = u_rect.xy + a_pos * u_rect.zw;
    vec2 ndc = vec2(px.x / u_screen.x * 2.0 - 1.0, 1.0 - px.y / u_screen.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}

/* FS_TEX — (a) window content / wallpaper / cursor */
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
void main() { gl_FragColor = texture2D(u_tex, v_uv); }

/* FS_SOLID — (b) focus ring / flat fills */
precision mediump float;
uniform vec4 u_color;
void main() { gl_FragColor = u_color; }

/* FS_BLUR — (c) one Gaussian pass; u_dir = (1/w,0)*step or (0,1/h)*step */
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_dir;
void main() {
    vec4 c = texture2D(u_tex, v_uv) * 0.227027;
    c += texture2D(u_tex, v_uv + u_dir * 1.0) * 0.1945946;
    c += texture2D(u_tex, v_uv - u_dir * 1.0) * 0.1945946;
    c += texture2D(u_tex, v_uv + u_dir * 2.0) * 0.1216216;
    c += texture2D(u_tex, v_uv - u_dir * 2.0) * 0.1216216;
    c += texture2D(u_tex, v_uv + u_dir * 3.0) * 0.054054;
    c += texture2D(u_tex, v_uv - u_dir * 3.0) * 0.054054;
    c += texture2D(u_tex, v_uv + u_dir * 4.0) * 0.016216;
    c += texture2D(u_tex, v_uv - u_dir * 4.0) * 0.016216;
    gl_FragColor = c;
}

/* FS_GLASS — (d) rounded-corner glass over the pre-blurred backdrop.
 * u_bd_rect maps this window's rect into the backdrop texture's UV space. */
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_backdrop;
uniform vec4 u_bd_rect;       /* backdrop-UV: x,y,w,h of the window rect */
uniform vec2 u_size_px;       /* window w,h in pixels */
uniform float u_radius_px;    /* corner radius (from nw_compose_set_theme getter) */
uniform vec4 u_tint;          /* glass tint rgb + body alpha */
void main() {
    vec2 p = v_uv * u_size_px;
    vec2 k = min(p, u_size_px - p);              /* distance to nearest corner box */
    float r = u_radius_px;
    vec2 cq = max(vec2(r) - k, vec2(0.0));
    float d = length(cq);                        /* d > r means outside the rounded corner */
    float mask = 1.0 - smoothstep(r - 1.0, r + 1.0, d);   /* 1px anti-aliased edge */
    vec4 bd = texture2D(u_backdrop, u_bd_rect.xy + v_uv * u_bd_rect.zw);
    vec4 col = mix(bd, vec4(u_tint.rgb, 1.0), u_tint.a);
    gl_FragColor = vec4(col.rgb, mask);
}
```

Blend state for glass/window draws: `glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);` — content quads for opaque windows draw with blend off.
Resources: per-window one `GL_RGBA` texture sized to the window (recreate on resize), `glTexSubImage2D` on damage (`nw_gl_window_dirty` marks; upload lazily in `nw_gl_frame`; NanOS window buffers are `0x00RRGGBB` — upload as `GL_BGRA_EXT` if the ES context exposes `EXT_texture_format_BGRA8888` (virgl and iris both do; check the extension string at init and fail to CPU if absent, logging it) so no CPU swizzle); wallpaper: one texture uploaded once from `wall`; two half-res FBO+texture pairs for the blur ping-pong; one small cursor texture built from the existing cursor bitmap.

- [ ] **Step 2: Frame loop**

Skeleton (uniform/location caching, error paths and the texture-upload helper omitted here — write them; the STRUCTURE below is the contract):

```c
int nw_gl_frame(const struct nw_server *s, const struct nw_surface *wall)
{
    glViewport(0, 0, g.sw, g.sh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);            /* the GBM-backed default fb */
    draw_tex(g.wall_tex, rect(0, 0, g.sw, g.sh), 0); /* wallpaper, blend off */

    /* back-to-front — same z-order walk as nw_compose_scene */
    for (int i = 0; i < s->win_count; i++) {
        const struct nw_win *w = zorder_at(s, i);    /* the shared walk helper */
        if (win_tex_dirty(w)) upload_win_tex(w);     /* glTexSubImage2D, damaged rows */
        if (w->glass) {
            /* 1. snapshot the scene under the window into blur ping-pong A (half-res) */
            grab_backdrop(w->rect);   /* glCopyTexSubImage2D from fb 0 into A */
            /* 2. two blur passes: A --(FS_BLUR,u_dir=x)--> B --(u_dir=y)--> A */
            blur_pass(g.fboB, g.texA, (float[]){ 1.0f / g.blur_w, 0 });
            blur_pass(g.fboA, g.texB, (float[]){ 0, 1.0f / g.blur_h });
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            /* 3. glass quad sampling A, blend on */
            draw_glass(w, g.texA);
        }
        draw_tex(win_tex(w), w->content_rect, w->glass /* blend if translucent */);
        if (w == s->focus) draw_solid(focus_ring_rect(w), g.accent_rgba);
    }
    draw_tex(g.cursor_tex, rect(s->mx, s->my, NW_CURSOR_W, NW_CURSOR_H), 1);
    if (glGetError() != GL_NO_ERROR) return -1;      /* session falls back to CPU */
    return glkms_swap(&g.kms);
}
```

The z-order walk: `nw_compose_scene` iterates the server's window list in its established order — extract that iteration into a tiny shared helper (`zorder_at(s, i)` above) used by BOTH paths, so GL and CPU can never disagree on stacking; keep the extraction minimal and covered by the Step-4 doctest.

- [ ] **Step 3: Wire into `nwm.c` + env kill-switch**

At nwm startup (where the fb surface is set up): `if (!getenv("NWM_NO_GL") && nw_gl_init(w, h) == 0) use GL each frame; else CPU`. Frame site: where nwm currently blits the backbuffer to fb0, call `nw_gl_frame` instead when active; any nonzero return → `nw_gl_shutdown()` + log `nwm: GL backend disabled, CPU fallback` + CPU path for the session. **`NWM_NO_GL=1` is the support/debug escape hatch — document it.**

- [ ] **Step 4: Host-test what is host-testable**

The scene→drawlist decisions (which windows blur, rect math) stay in already-host-tested pure code (`nw_backdrop.c` rect helpers). Do NOT try to host-test GL calls. Add one doctest only if you extracted the z-order walk helper (assert order + glass flags on a fake server) — file `tests/test_compose_walk.cpp`.

- [ ] **Step 5: `scripts/smoke-virtio-gpu-gl.sh`**

Model on `scripts/smoke-virtio-gpu.sh` (read it first; reuse its screendump + distinct-colours helpers):
1. Locate a virgl QEMU (`QEMU_GL` or the tap prefix); if absent → `echo "SKIP: no virgl-capable QEMU"; exit 0`.
2. Boot via `run64-gl.sh` headless.
3. Assert serial: `virgl 3D negotiated`; run `drmtest` → all OK markers; run `gles2info` → `clear-readback OK`.
4. Desktop: log in on the graphical VT (mirror how `smoke-vt`/`smoke-virtio-gpu` drive nwlogin), wait, assert serial has NO `GL backend disabled`, screendump → distinct-colours ≥ 200 AND serial contains `nwm: GL compositor active` (add that log line in Step 3's init-success path).
5. Assert no `EXCEPTION|PANIC|BUG:` in serial.

Makefile:

```make
smoke-virtio-gpu-gl: image64
	bash scripts/smoke-virtio-gpu-gl.sh
```

and append `smoke-virtio-gpu-gl` to the `verify64:` prerequisite list (`Makefile:922`).

- [ ] **Step 6: Full verification**

Run: `make verify64` on the machine with the GL QEMU.
Expected: everything green including the new gate; `smoke-virtio-gpu` (CPU/2D) unchanged — proving the fallback.
Also run an interactive `bash scripts/run64-gl.sh` session and drag a glass window around — the blur must track live with no cache seams (GPU path has no cache).

- [ ] **Step 7: Docs + commit**

`docs/en/graphics.md`: new section "DRM nodes + GL stack" (the /dev/dri ABI, mmapAt, libdrm/Mesa ports, GBM/KMS present, nwm GL backend + `NWM_NO_GL`, the QEMU tap setup). Mirror the key content in `docs/pl/graphics.md`; cross-ref from `docs/en/linuxkpi.md` §follow-ons. Flip this plan's design record status if listed.

```bash
git add user/nwm/ scripts/smoke-virtio-gpu-gl.sh Makefile docs/ tests/
git commit -m "nwm: GL ES compositor backend (GPU glass blur) + smoke-virtio-gpu-gl gate"
```

---

## Self-Review

**Spec coverage** (vs `2026-07-01-gpu-stack-gl-vulkan-dell-design.md`): real DRM ABI from day one → Tasks 3-5; kernel de-risk without Mesa → Task 6 (raw-ABI drmtest replaces the old bespoke emitter); libdrm+Mesa virgl, no LLVM → Tasks 7-8; canonical GBM/KMS present (carries to i915) → Task 9; nwm GL backend + permanent CPU fallback → Task 10; QEMU-on-M4 tooling → Task 1; gates SKIP without virgl QEMU → Tasks 1, 10. ✓

**Deliberate scope cuts** (recorded, not forgotten): page-flip events via `DrmDevice::read`/`poll` (Task 9 uses SetCrtc-per-swap first); per-fd (vs per-process) drm_file identity (Task 4 Step 4); prime/dma-buf cross-process sharing (apps still software-render into nwm shared buffers; GL apps other than nwm render offscreen). All three are prerequisites only for multi-client GL, which is not in this plan's success criterion.

**Known discovery points** (implementer resolves against vendored source, never by guessing): exact `drm_file_alloc`/minor fields (Task 4 Step 1); `drm_gem_shmem` pin/get-pages exact API (Task 5); Mesa 24.2 meson option spellings (Task 8 Step 2); the docker image's meson/ninja presence (Task 7); tap formula name drift (Task 1).

**Type consistency:** `knx_drm_ops` (ioctl/mmap_offset/release) is used identically in Tasks 3, 4, 5; `glkms` struct + `glkms_open/swap/close` shared by Tasks 9-10; `nw_gl_*` API defined once (Task 10). ✓
