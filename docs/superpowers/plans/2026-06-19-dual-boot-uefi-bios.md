# Dual-firmware boot (UEFI + BIOS) via Limine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended)
> or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make the NanOS x86_64 disk image a hybrid **GPT** stick that boots to a shell under **both
UEFI and legacy BIOS** using the **Limine** bootloader, with the Multiboot1 kernel unchanged, verified
end-to-end in QEMU under both firmwares.

**Architecture:** Replace the BIOS-only hand-rolled GRUB install with Limine (prebuilt BIOS stages +
`BOOTX64.EFI`, deploys onto a flat image file with no loop device). The image becomes GPT: a BIOS-boot
partition (Limine BIOS stages), a FAT ESP (`BOOTX64.EFI` + `limine.conf`), and the ext4 root (the whole
NanOS system, label `NANOS`). The kernel gains GPT partition parsing so it finds the ext4 root on the
now-GPT disk. Spec: `docs/superpowers/specs/2026-06-19-dual-boot-uefi-bios-design.md`.

**Tech Stack:** Limine (`v8.x-binary` branch), `parted` (GPT), `mtools` (FAT ESP, no mount),
`mke2fs -E offset` (ext4), `debugfs` (populate), QEMU `qemu-system-x86_64` with SeaBIOS (BIOS) and
edk2/OVMF (UEFI). C++ freestanding kernel (Multiboot1, unchanged). doctest host tests for the MI GPT
parser (`make ARCH=x86_64 test`, ≥90% lcov gate).

**Conventions to follow (read first):**
- Build runs in the `nanos-build` Docker image; QEMU runs natively on the macOS host.
  `make image64` builds the x86_64 image; `make ARCH=x86_64 test` runs host tests; `make smoke-x86_64`
  is the BIOS boot smoke.
- MI code reaches hardware only through `<arch/...>`; `make check-arch` fails on x86 leaks in
  `init/kernel/mm/fs/lib/drivers/usb`. The GPT parser is MI (a `BlockDevice` consumer) — keep it clean.
- No global constructors run; use zeroed `.bss` / `new` / function statics.
- The image's ext4 root partition byte offset is **fixed at 35651584** (34 MiB) by this plan's layout —
  it is a constant, not computed at runtime.

---

## File Structure (created/modified)

**New (MI, host-tested):**
- `kernel/PartitionTable.h` / `kernel/PartitionTable.cpp` — `firstFsPartitionLba(BlockDevice*)`: MBR +
  GPT partition discovery, returns the start LBA of the first ext partition.
- `tests/test_partitiontable.cpp` — doctest over crafted MBR and GPT images in a `RamBlockDevice`.

**New (build/verify):**
- `scripts/smoke-uefi.sh` — UEFI boot smoke via OVMF on the host.

**Modified:**
- `docker/Dockerfile` — trailing layer: fetch Limine `v8.x-binary`, build the `limine` host tool,
  install `BOOTX64.EFI` + `limine`.
- `scripts/create-image.sh` — rewrite the skeleton builder for GPT + Limine (keep the filename so
  the Makefile call site is unchanged).
- `kernel/Kernel.cpp` — `firstPartitionLba()` delegates to `firstFsPartitionLba()`.
- `Makefile` — `IMAGE64_GRUB2_PART` offset `1048576` → `35651584`; `MI_SOURCES`/`TEST_MODULES`/
  `COV_PATTERNS` add `PartitionTable`; `_image64` drops the obsolete GRUB `grub.cfg` step; `smoke-uefi`
  target + `verify64` wiring.

---

## Task 1: Add Limine tooling to the build image

**Files:**
- Modify: `docker/Dockerfile` (add a trailing layer, after the picolibc build, before `WORKDIR /src`)

> No host unit test — this provisions the toolchain. Verification = the image rebuilds and the `limine`
> tool + `BOOTX64.EFI` are present. The trailing position keeps the cross-toolchain cache layers valid
> (same pattern as the grub-pc-bin / lcov layers).

- [ ] **Step 1: Add the Limine layer.** Insert before the final `WORKDIR /src`:

```dockerfile
# Limine bootloader (BIOS+UEFI) for the hybrid GPT image. The vN.x-binary branch ships prebuilt
# BIOS stages + BOOTX64.EFI and a tiny host `limine` deploy tool we build with the host cc. Fetched
# via wget+tar (no git in the image). Trailing layer so the toolchain cache above stays valid.
RUN set -eux; cd /tmp; \
    wget -qO limine.tar.gz https://github.com/limine-bootloader/limine/archive/refs/heads/v8.x-binary.tar.gz; \
    d="$(tar tzf limine.tar.gz | head -1 | cut -d/ -f1)"; \
    tar xf limine.tar.gz; \
    cd "$d"; \
    make; \
    install -Dm644 BOOTX64.EFI    /usr/local/share/limine/BOOTX64.EFI; \
    install -Dm755 limine          /usr/local/bin/limine; \
    cd /; rm -rf /tmp/limine* /tmp/"$d"
```
(The tarball's top dir is `Limine-8.x-binary` — capital L, no `v` — so derive it dynamically.)

- [ ] **Step 2: Rebuild the build image.** The build context is `docker/` (where the COPY'd
  picolibc cross-files live), as the Makefile does it.

Run: `docker build -t nanos-build docker/`
Expected: builds to completion; the Limine layer is the only one re-run if the toolchain was cached.

- [ ] **Step 3: Verify the tool + EFI binary are present.**

Run: `docker run --rm nanos-build sh -c 'limine version && ls -l /usr/local/share/limine/BOOTX64.EFI'`
Expected: prints a Limine version string and lists a non-empty `BOOTX64.EFI`.

- [ ] **Step 4: Commit.**

```bash
git add docker/Dockerfile
git commit -m "build: add Limine (v8.x-binary) — host deploy tool + BOOTX64.EFI for the hybrid image"
```

---

## Task 2: Kernel GPT partition discovery (MI, host-tested)

**Files:**
- Create: `kernel/PartitionTable.h`, `kernel/PartitionTable.cpp`, `tests/test_partitiontable.cpp`
- Modify: `Makefile` (`MI_SOURCES`, `TEST_MODULES`, `COV_PATTERNS`)

- [ ] **Step 1: Write the header `kernel/PartitionTable.h`.**

```cpp
// kernel/PartitionTable.h — find the NanOS root partition's start LBA on an MBR or GPT disk (MI).
#ifndef PARTITION_TABLE_H
#define PARTITION_TABLE_H
#include "BlockDevice.h"
namespace kernel {
// Returns the start LBA of the first partition that looks like an ext filesystem (ext magic 0xEF53
// at the partition's superblock). Handles a protective-MBR/GPT disk and a classic MBR. Falls back to
// LBA 2048 when nothing matches (the historical default). Reads only via the BlockDevice.
unsigned firstFsPartitionLba(BlockDevice* dev);
}
#endif
```

- [ ] **Step 2: Write the failing test `tests/test_partitiontable.cpp`.** Builds a protective-MBR + GPT
  image with two partitions (a non-ext one and the ext root) in a `RamBlockDevice`, and a classic-MBR
  image, and checks discovery picks the ext partition / the MBR partition.

```cpp
#include "doctest.h"
#include "PartitionTable.h"
#include "RamBlockDevice.h"
#include <string.h>
using namespace kernel;

static void put32(unsigned char* p, unsigned v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put64(unsigned char* p, unsigned long long v){ for(int i=0;i<8;i++) p[i]=(unsigned char)(v>>(8*i)); }

TEST_CASE("GPT: discovery returns the ext partition's start LBA") {
    static unsigned char buf[64*512];
    memset(buf, 0, sizeof buf);
    // Protective MBR: one entry of type 0xEE at 0x1BE.
    buf[0x1BE + 4] = 0xEE;
    buf[510] = 0x55; buf[511] = 0xAA;
    // GPT header @ LBA1: "EFI PART", PartitionEntryLBA=2 (off 72), NumEntries=4 (off 80), EntrySize=128 (off 84).
    unsigned char* h = buf + 512;
    memcpy(h, "EFI PART", 8);
    put64(h + 72, 2); put32(h + 80, 4); put32(h + 84, 128);
    // Entry array @ LBA2: entry0 = non-ext (start=10), entry1 = ext root (start=34).
    unsigned char* e = buf + 2*512;
    e[0] = 0x11;            put64(e + 32, 10);                 // entry0 type nonzero, StartingLBA=10
    e[128 + 0] = 0x22;      put64(e + 128 + 32, 34);           // entry1 type nonzero, StartingLBA=34
    // ext superblock magic 0xEF53 for the root: byte 1080 of the partition = sector (34+2), offset 56.
    unsigned char* sb = buf + (34 + 2) * 512;
    sb[56] = 0x53; sb[57] = 0xEF;
    // (sector 10+2 left zero -> entry0 is not ext)
    RamBlockDevice dev("gpt", buf, sizeof buf);
    CHECK(firstFsPartitionLba(&dev) == 34);
}

TEST_CASE("MBR: discovery returns the first partition's start LBA") {
    static unsigned char buf[8*512];
    memset(buf, 0, sizeof buf);
    buf[510] = 0x55; buf[511] = 0xAA;
    unsigned char* p = buf + 0x1BE;          // first partition entry
    p[4] = 0x83;                              // type Linux (non-zero, not 0xEE)
    put32(p + 8, 2048);                       // StartingLBA = 2048
    RamBlockDevice dev("mbr", buf, sizeof buf);
    CHECK(firstFsPartitionLba(&dev) == 2048);
}
```

- [ ] **Step 3: Run it; expect FAIL** (`PartitionTable.h` / `firstFsPartitionLba` undefined).

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "partitiontable|firstFsPartitionLba|error"`
Expected: compile error — `PartitionTable.h` not found.

- [ ] **Step 4: Implement `kernel/PartitionTable.cpp`.**

```cpp
// kernel/PartitionTable.cpp — MBR + GPT partition discovery (MI). See PartitionTable.h.
#include "PartitionTable.h"
#include <string.h>
namespace kernel {

static unsigned long long rd64(const unsigned char* p) {
    unsigned long long v = 0; for (int i = 0; i < 8; i++) v |= (unsigned long long)p[i] << (8 * i); return v;
}
static unsigned rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

// True if the partition starting at `startLba` carries an ext superblock (magic 0xEF53). The ext
// superblock is at byte 1024 of the partition; its s_magic is at superblock offset 56 -> absolute
// byte 1080 = sector (startLba+2), offset 56.
static bool looksExt(BlockDevice* dev, unsigned long long startLba) {
    unsigned char s[512];
    if (dev->readSectors(startLba + 2, 1, s) != 0) return false;
    return s[56] == 0x53 && s[57] == 0xEF;
}

unsigned firstFsPartitionLba(BlockDevice* dev) {
    unsigned char mbr[512];
    if (dev->readSectors(0, 1, mbr) != 0) return 2048;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 2048;

    // GPT? A protective MBR has a single partition entry of type 0xEE.
    bool gpt = false;
    for (int i = 0; i < 4; i++) if (mbr[0x1BE + i * 16 + 4] == 0xEE) gpt = true;

    if (gpt) {
        unsigned char hdr[512];
        if (dev->readSectors(1, 1, hdr) != 0) return 2048;
        if (memcmp(hdr, "EFI PART", 8) != 0) return 2048;
        unsigned long long entryLba = rd64(hdr + 72);
        unsigned numEntries = rd32(hdr + 80);
        unsigned entrySize  = rd32(hdr + 84);
        if (entrySize < 128 || entrySize > 512 || numEntries > 256) return 2048;
        unsigned char sec[512];
        unsigned perSec = 512 / entrySize;
        for (unsigned i = 0; i < numEntries; i++) {
            if (i % perSec == 0)
                if (dev->readSectors(entryLba + i / perSec, 1, sec) != 0) return 2048;
            const unsigned char* e = sec + (i % perSec) * entrySize;
            // Skip an unused entry (all-zero type GUID).
            bool used = false; for (int b = 0; b < 16; b++) if (e[b]) used = true;
            if (!used) continue;
            unsigned long long start = rd64(e + 32);   // StartingLBA
            if (looksExt(dev, start)) return (unsigned)start;
        }
        return 2048;
    }

    // Classic MBR: first non-empty partition entry.
    for (int i = 0; i < 4; i++) {
        const unsigned char* e = mbr + 0x1BE + i * 16;
        unsigned type = e[4];
        unsigned start = rd32(e + 8);
        if (type != 0 && start != 0) return start;
    }
    return 2048;
}

}  // namespace kernel
```

- [ ] **Step 5: Wire the Makefile.**
  - `MI_SOURCES += PartitionTable.o` (append to an existing `MI_SOURCES+=` line, e.g. the `UsbCore.o` one).
  - `TEST_MODULES += kernel/PartitionTable.cpp`
  - `COV_PATTERNS` add `"*/PartitionTable.*"`.

- [ ] **Step 6: Run the test; expect PASS.**

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "test cases|PartitionTable|fail"`
Expected: both partitiontable cases pass; suite green ≥90%.

- [ ] **Step 7: Commit.**

```bash
git add kernel/PartitionTable.h kernel/PartitionTable.cpp tests/test_partitiontable.cpp Makefile
git commit -m "feat(boot): MI GPT+MBR partition discovery (firstFsPartitionLba, host-tested)"
```

---

## Task 3: Wire the kernel to GPT discovery

**Files:**
- Modify: `kernel/Kernel.cpp` (`firstPartitionLba` — around line 69; include + delegate)

- [ ] **Step 1: Include the parser.** Near the other `#include` lines at the top of `kernel/Kernel.cpp`:

```cpp
#include "PartitionTable.h"     // MBR + GPT root-partition discovery
```

- [ ] **Step 2: Delegate `firstPartitionLba` to the MI parser.** Replace the body of the existing
  `static unsigned firstPartitionLba(BlockDevice* dev)` (the MBR-only scan) with a single call:

```cpp
static unsigned firstPartitionLba(BlockDevice* dev) {
    return firstFsPartitionLba(dev);   // MBR + GPT (the image is GPT after the Limine switch)
}
```

- [ ] **Step 3: Build the kernel; expect success + arch-clean.**

Run: `make check-arch && make image64 2>&1 | tail -3`
Expected: "OK: MI layer is arch-clean." and a built image (the image is still the OLD MBR layout until
Task 4 — but the kernel now also handles MBR, so the existing image still boots; verified next).

- [ ] **Step 4: Regression — the current (MBR) image still boots to a shell.**

Run: `make smoke-x86_64`
Expected: PASS (the MBR path of `firstFsPartitionLba` still finds the root at LBA 2048).

- [ ] **Step 5: Commit.**

```bash
git add kernel/Kernel.cpp
git commit -m "feat(boot): root discovery uses MBR+GPT partition table (firstFsPartitionLba)"
```

---

## Task 4: GPT + Limine image builder (the dual-boot image)

**Files:**
- Modify: `scripts/create-image.sh` (rewrite for GPT + Limine; keep the filename)
- Modify: `Makefile` (`IMAGE64_GRUB2_PART` offset; drop the GRUB `grub.cfg` step in `_image64`)

> Verification = QEMU under both firmwares (Task 5). This task produces the image; Task 5 asserts it.

- [ ] **Step 1: Rewrite `scripts/create-image.sh`.** GPT (bios_boot + ESP + ext4 root), no loop
  device. Replaces the MBR + GRUB-i386-pc flow.

```bash
#!/bin/bash
# Creates a hybrid GPT disk image bootable under BOTH BIOS and UEFI via Limine, with a writable ext4
# root (label NANOS). Runs INSIDE nanos-build (Linux); no Docker, no mount, no loop device.
# Layout (parted aligns to 1 MiB): P1 bios_boot @1MiB(1MiB), P2 ESP/FAT @2MiB(32MiB), P3 ext4 root @34MiB.
set -e
IMAGE_PATH="${IMAGE_PATH:-disk/image.img}"
ROOT_OFFSET=35651584    # 34 MiB — the ext4 root partition (P3) byte offset (must match the Makefile)

if [ -f "$IMAGE_PATH" ]; then
    echo "Image $IMAGE_PATH already exists. Delete it to recreate."
    exit 0
fi
mkdir -p "$(dirname "$IMAGE_PATH")"

# 320 MiB disk (root ~286 MiB after the 34 MiB boot area).
dd if=/dev/zero of="$IMAGE_PATH" bs=1M count=320 status=none

# GPT: bios_boot (Limine BIOS stages), ESP (FAT, Limine UEFI), ext4 root.
parted -s "$IMAGE_PATH" mklabel gpt
parted -s "$IMAGE_PATH" mkpart bios_boot 1MiB 2MiB
parted -s "$IMAGE_PATH" set 1 bios_grub on
parted -s "$IMAGE_PATH" mkpart ESP fat16 2MiB 34MiB
parted -s "$IMAGE_PATH" set 2 esp on
parted -s "$IMAGE_PATH" mkpart NANOS ext4 34MiB 100%

# --- ESP (FAT) built in a separate file with mtools, then dd'd into P2 (offset 2 MiB) ---
dd if=/dev/zero of=/tmp/esp.img bs=1M count=32 status=none
mformat -i /tmp/esp.img -v ESP ::
mmd -i /tmp/esp.img ::/EFI ::/EFI/BOOT
mcopy -i /tmp/esp.img /usr/local/share/limine/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
cat > /tmp/limine.conf <<'LCONF'
timeout: 0
/NanOS
    protocol: multiboot1
    path: fslabel(NANOS):/nanos/core/kernel.bin
LCONF
mcopy -i /tmp/esp.img /tmp/limine.conf ::/EFI/BOOT/limine.conf
dd if=/tmp/esp.img of="$IMAGE_PATH" bs=1M seek=2 conv=notrunc status=none

# --- ext4 root at P3 (label NANOS so limine.conf's fslabel(NANOS) resolves it) ---
ROOT_BLOCKS=$(( (320*1024*1024 - ROOT_OFFSET) / 1024 ))
mke2fs -t ext4 -q -L NANOS -E offset=$ROOT_OFFSET "$IMAGE_PATH" ${ROOT_BLOCKS}k

# --- Limine BIOS stages into the bios_boot partition (P1) ---
limine bios-install "$IMAGE_PATH" 1

rm -f /tmp/esp.img /tmp/limine.conf
echo "Hybrid GPT+Limine image created: $IMAGE_PATH (root @ $ROOT_OFFSET)"
```

- [ ] **Step 2: Point the Makefile populate at the new root offset.** Change the x86_64 partition spec:

In `Makefile`, replace:
```make
IMAGE64_GRUB2_PART=$(IMAGE64_GRUB2)?offset=1048576
```
with:
```make
IMAGE64_GRUB2_PART=$(IMAGE64_GRUB2)?offset=35651584
```

- [ ] **Step 3: Drop the obsolete GRUB grub.cfg step from `_image64`.** Limine reads `limine.conf` from
  the ESP, not `/boot/grub/grub.cfg` on ext4. Remove these two lines from the `_image64:` recipe:
```make
	@printf 'set timeout=0\nset default=0\nmenuentry "NanOS x86_64" {\n  multiboot /nanos/core/kernel.bin\n}\n' > /tmp/grub64.cfg
	printf "rm /boot/grub/grub.cfg\nwrite /tmp/grub64.cfg /boot/grub/grub.cfg\n" | debugfs -w "$(IMAGE64_GRUB2_PART)"
```
(Leave the rest of `_image64` — the `/nanos` tree + kernel population via `debugfs` — unchanged; it now
writes into the GPT ext4 root via the new offset.)

- [ ] **Step 4: Build the image and inspect the partition table + ESP.**

```bash
make image64
docker run --rm -v "$PWD":/src nanos-build sh -c \
  'parted -s disk/image64.img print; \
   echo "--- ESP /EFI/BOOT ---"; mdir -i disk/image64.img@@2M ::/EFI/BOOT'
```
Expected: `parted print` shows a `gpt` label with `bios_boot`, `esp`, and a `NANOS`/ext4 partition;
the kernel + `/nanos` tree are in the ext4 root (the build's `debugfs` steps ran without error).

- [ ] **Step 5: Commit.**

```bash
git add scripts/create-image.sh Makefile
git commit -m "feat(boot): hybrid GPT image built with Limine (bios_boot + ESP/FAT + ext4 root)"
```

---

## Task 5: Verify both firmwares in QEMU + wire the gate

**Files:**
- Create: `scripts/smoke-uefi.sh`
- Modify: `Makefile` (`smoke-uefi` target; `verify64` wiring)

- [ ] **Step 1: BIOS boot smoke on the new GPT image.**

Run: `make smoke-x86_64`
Expected: PASS — SeaBIOS runs the Limine BIOS stage from the bios_boot partition → Limine → kernel →
`bash-5`, zero faults. (Proves the GPT layout + `firstFsPartitionLba` GPT path + Limine BIOS + the
Multiboot1 handoff all work.)

- [ ] **Step 2: Write `scripts/smoke-uefi.sh`** (UEFI via OVMF on the host).

```bash
#!/usr/bin/env bash
# smoke-uefi.sh — UEFI boot smoke: boot the GPT image with edk2/OVMF firmware (Homebrew qemu) so the
# firmware runs EFI/BOOT/BOOTX64.EFI (Limine) -> kernel -> shell. Asserts the shell is reached and the
# Multiboot1+GOP-framebuffer handoff produced zero faults. Pairs with smoke-x86_64 (BIOS/SeaBIOS).
set -u
IMG=disk/image64.img
SER=/tmp/nanos-uefi-serial.log
INT=/tmp/nanos-uefi-int.log
VARS=/tmp/nanos-uefi-vars.fd
rm -f "$SER" "$INT" "$VARS"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }

# Locate edk2/OVMF firmware (ships with Homebrew qemu).
QDIR="$(dirname "$(command -v qemu-system-x86_64)")/../share/qemu"
CODE=""
for c in "$QDIR/edk2-x86_64-code.fd" "$QDIR/edk2-x86_64-secure-code.fd" /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$c" ] && CODE="$c" && break
done
[ -n "$CODE" ] || { echo "FAIL: edk2/OVMF firmware not found (looked in $QDIR)"; exit 2; }
# Writable vars: prefer the shipped template, else a blank 64 MiB pflash.
VARSRC=""; for v in "$QDIR/edk2-i386-vars.fd" "$QDIR/edk2-x86_64-vars.fd" /usr/share/OVMF/OVMF_VARS.fd; do
    [ -f "$v" ] && VARSRC="$v" && break
done
if [ -n "$VARSRC" ]; then cp "$VARSRC" "$VARS"; else dd if=/dev/zero of="$VARS" bs=1m count=64 2>/dev/null; fi

pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -m 512 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 45); do grep -q "bash-5\|starting shell" "$SER" 2>/dev/null && break; sleep 1; done
sleep 2

PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; grep -E "$1" "$SER"|head -3|sed 's/^/        /'; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== UEFI (OVMF) boot smoke ==="
chk "Mounting ext filesystem at /disks/main"  "ext root mounted (GPT discovery worked under UEFI)"
chk "bash-5"                                  "reached the login shell via BOOTX64.EFI -> Limine -> kernel"
no  "CPU EXCEPTION|KERNEL EXCEPTION|killed faulting process|Triple fault"  "no faults on the console"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: QEMU logged a Triple fault"; PASS=0; else echo "  OK  : no triple fault in QEMU int log"; fi
echo "================================"
if [ "$PASS" = 1 ]; then echo "UEFI boot smoke: PASS"; exit 0; else echo "UEFI boot smoke: FAIL"; exit 1; fi
```

- [ ] **Step 3: Add the `smoke-uefi` target + wire `verify64`.** In `Makefile`, add `smoke-uefi` to the
  `.PHONY` line with the other smokes, then:
```make
smoke-uefi: image64
	bash scripts/smoke-uefi.sh
```
and change the `verify64` target line to:
```make
verify64: test64 smoke-x86_64 smoke-uefi smoke-usb
	@echo "x86_64 verify: host tests + BIOS + UEFI + live-USB boot smokes all passed."
```

- [ ] **Step 4: Run the UEFI smoke; expect PASS.**

Run: `chmod +x scripts/smoke-uefi.sh && make smoke-uefi`
Expected: ext root mounts, `bash-5` reached, zero faults. This is the moment the **Multiboot1 handoff
from Limine** and the **GOP framebuffer** are proven. (If the framebuffer faults here, that is the
early signal for piece 3 — note it and continue; the shell-reached assertion is the gate.)

- [ ] **Step 5: Commit.**

```bash
git add scripts/smoke-uefi.sh Makefile
git commit -m "test(boot): smoke-uefi (OVMF) — UEFI boot gate; verify64 now covers BIOS+UEFI+USB"
```

---

## Notes for the implementer

- **Limine docs were consulted** (context7 `/limine-bootloader/limine`): the real config keys are
  `protocol:` + `path:` (not `kernel_path:`); GPT BIOS boot **requires** a bios_boot partition and
  `limine bios-install <image> <partno>`; `fslabel(NAME)` resolves a partition by ext label; the
  multiboot1 info Limine passes carries the `fb_*` and `mmap_*` fields `bootinfo_x86_64.cpp` reads.
- **Pin Limine if reproducibility matters.** The plan uses the `v8.x-binary` rolling branch (always
  exists). To freeze, replace the branch with a tag tarball URL in Task 1.
- **The integration risk is the Limine→bootinfo Multiboot1 handoff.** Both smokes (Step 1 BIOS, Step 4
  UEFI) are the test; the framebuffer console showing boot text + `bash-5` is the signal. If the kernel
  faults immediately, dump `/tmp/nanos-*-serial.log` and check whether `bootinfo_x86_64.cpp` got a valid
  framebuffer/mmap from Limine's MB1 info (vs GRUB's).
- **OVMF on macOS:** `smoke-uefi.sh` auto-locates `edk2-x86_64-code.fd` from the Homebrew qemu install;
  if QEMU was installed elsewhere, set the firmware path by editing the `for c in ...` list.
- **Out of scope (do not pull in):** real-HW xHCI handoff (piece 2), real-RAM memory map (piece 3),
  NVMe, i686 image changes, Secure Boot.
```
