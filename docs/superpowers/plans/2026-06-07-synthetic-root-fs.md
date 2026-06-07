# Synthetic Root Filesystem — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use `- [ ]`.

**Goal:** `/` becomes an in-memory `SynthFs` (dirs `disks`/`dev`/`proc`); the physical
disk mounts at `/disks/main`; `/dev/{null,zero,random}` + `/proc/uptime` are generated.

**Architecture:** New `SynthFs` (a `FileSystem`) holding a `SynthNode` tree, plugged into
the existing prefix-routing `Vfs`. A `Vfs::mount(mp, FileSystem*)` overload mounts the
device-less synth root. `mountVolume()` ties a disk mount to a `/disks/<name>` marker.

**Spec:** `docs/superpowers/specs/2026-06-07-synthetic-root-fs-design.md`
**Branch/commits:** `dockerized-build`, one commit per task, no Claude attribution.

---

## Task 1: SynthFs core + node model (host-tested)

**Files:** Create `fs/SynthFs.h`, `fs/SynthFs.cpp`, `tests/test_synthfs.cpp`; modify
`Makefile` (`MI_SOURCES += SynthFs.o`, `TEST_MODULES`/`COV_PATTERNS`).

- [ ] **Step 1 — failing test** `tests/test_synthfs.cpp`: build a SynthFs, assert
  `readdir("/")` lists `disks/dev/proc`; `stat("/")`/`stat("/dev")` are `NODE_DIR`;
  a static node read returns its bytes (offset/partial); `addVolume("main")` →
  `readdir("/disks")` lists `main`; missing path errors.
- [ ] **Step 2 — run, confirm fail** (`make test` → `SynthFs.h` not found).
- [ ] **Step 3 — implement `fs/SynthFs.h`/`.cpp`** (node model: DIR/STATIC/GEN; path
  walk; `read/stat/readdir`; builds `/`,`/disks`,`/dev`,`/proc`; `addVolume`,
  `addStatic`, `addGen`). Code in the spec + below.
- [ ] **Step 4 — run, confirm pass** (`make test`, ≥90%).
- [ ] **Step 5 — commit** `feat: SynthFs in-memory virtual filesystem (host-tested)`.

Node model + walk (concrete):

```cpp
// fs/SynthFs.h
#pragma once
#include "Vfs.h"
namespace kernel {
typedef int (*SynthGen)(unsigned off, void* buf, unsigned n);
enum SynthKind { SK_DIR, SK_STATIC, SK_GEN };
struct SynthNode {
    char name[64];
    SynthKind kind;
    SynthNode* child[32]; int nchild;     // DIR
    const char* data; unsigned len;       // STATIC
    SynthGen gen;                         // GEN
    unsigned mode;                        // stat mode bits
};
class SynthFs : public FileSystem {
    SynthNode* root;
    SynthNode* mk(SynthKind k, const char* name, unsigned mode);
    SynthNode* dirChild(SynthNode* d, const char* name, int len);
    SynthNode* walk(const char* path);    // 0 if missing
    SynthNode* m_disks; SynthNode* m_dev; SynthNode* m_proc;
public:
    SynthFs();
    SynthNode* addDir(SynthNode* parent, const char* name);
    void addStatic(SynthNode* parent, const char* name, const char* data, unsigned len);
    void addGen(SynthNode* parent, const char* name, SynthGen g, unsigned mode);
    void addVolume(const char* name);     // marker dir under /disks
    SynthNode* dev() { return m_dev; }
    SynthNode* proc() { return m_proc; }
    int mount();
    int read(String path, unsigned size, unsigned off, void* buf);
    int stat(String path, FileStat& out);
    int readdir(String path, List<DirEntry>& out);
};
}
```

`walk`: split `path` on `/`; from `root`, for each component find a matching DIR child;
return the node or 0. `read`: STATIC → copy `[off, off+size)` clamped to `len`; GEN →
`gen(off,buf,size)`; DIR → -1. `stat`: DIR→NODE_DIR, STATIC→NODE_FILE size=len,
GEN→NODE_FILE size=0; set `out.mode/nlink=1/uid=gid=0/mtime=0`. `readdir`: DIR→list
children (name,type); else -1. Children stored in a fixed array (≤32, ample).

---

## Task 2: `Vfs::mount(mountpoint, FileSystem*)` overload

**Files:** modify `fs/Vfs.h`, `fs/Vfs.cpp`; extend `tests/test_vfs.cpp`.

- [ ] **Step 1 — failing test**: register a fake FS via the new overload, assert routing
  reaches it.
- [ ] **Step 2 — run, confirm fail.**
- [ ] **Step 3 — implement**: factor the `Mount{mp,fs}` insertion into a private
  `addMount(mountpoint, fs)`; the existing `mount(...,fstype,dev,lba)` calls it after
  `create()`; the new `int Vfs::mount(String mp, FileSystem* fs)` calls
  `fs->mount()` then `addMount(mp, fs)`.
- [ ] **Step 4 — run, confirm pass.**
- [ ] **Step 5 — commit** `feat: Vfs::mount overload for a pre-built FileSystem`.

---

## Task 3: Boot wiring + mountVolume + paths

**Files:** modify `kernel/Kernel.cpp` (build SynthFs, mount `/`, `mountVolume("main")`);
`user/nsh.c` only if a default path assumed the old layout (bare `ls` stays `/`).

- [ ] **Step 1 — implement**: in `Kernel::start`, `SynthFs* root = new SynthFs();`
  `vfs->mount("/", root);` then `mountVolume(vfs, root, "main", hd0, 2048);` (helper in
  Kernel.cpp: `vfs->mount("/disks/main","auto",dev,2048); root->addVolume("main");`).
  Keep `registerType` calls.
- [ ] **Step 2 — build** (`make build`), `make check-arch`, `make test`.
- [ ] **Step 3 — QEMU** (timeout=0, `scripts/qemu-shell.sh`): `ls /` → disks/dev/proc;
  `ls /disks` → main; `ls -l /disks/main/boot`; `cat /disks/main/boot/grub/grub.cfg`.
  No faults. Restore grub.cfg.
- [ ] **Step 4 — commit** `feat: synthetic root; mount the disk at /disks/main`.

---

## Task 4: /dev + /proc generators

**Files:** modify `fs/SynthFs.cpp` (or a small `fs/SynthNodes.cpp`) for the generators;
wire them in `SynthFs()` ctor (or via `addGen` from Kernel). Extend `tests/test_synthfs.cpp`.

- [ ] **Step 1 — failing test**: `/dev/zero` read returns N zero bytes; `/dev/null`
  returns 0; `/dev/random` returns N bytes and two reads differ; `/proc/uptime` read
  returns nonempty text.
- [ ] **Step 2 — run, confirm fail.**
- [ ] **Step 3 — implement generators**: `null`→return 0; `zero`→memset 0, return n;
  `random`→xorshift32 stream (seed varies by a static counter), return n;
  `uptime`→render a counter as decimal text (no timer yet; documented placeholder).
  Register under `dev()`/`proc()` in the ctor with mode bits (`0666` char-ish / `0444`).
- [ ] **Step 4 — run, confirm pass** (`make test`).
- [ ] **Step 5 — QEMU**: `cat /proc/uptime`, `cat /dev/zero | head`-equivalent (bounded).
- [ ] **Step 6 — commit** `feat: /dev/{null,zero,random} + /proc/uptime synth nodes`.

---

## Self-review
- Spec coverage: SynthFs (T1), Vfs overload (T2), boot/disks (T3), dev/proc (T4) — all mapped.
- Routing: `/` synth gets full path; `/disks/main/...` taken by the longer ext prefix; `/disks`
  (no mount) falls to synth → lists volume markers. Verified against Vfs::resolve.
- No placeholders; SynthNode field/method names consistent across tasks.
- Coverage: SynthFs.cpp gated; generators host-tested.
```
