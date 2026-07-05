# Real-hardware boot bring-up — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended)
> or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Close the two real-silicon gaps that block booting NanOS on the Dell from USB — a 64-bit
memory map with a 2 MiB huge-page kernel identity map (so >4 GiB RAM is used and high mmap ranges
aren't mis-cast), and the xHCI BIOS/firmware handoff (so our driver owns the controller on real HW).

**Architecture:** Component A (memory map) widens the boot-memory path to `uint64_t` end-to-end and adds
`AddressSpace::mapRangeHuge` (2 MiB PD entries) for the kernel identity map; fully QEMU-verifiable
(`-m 6144`/`-m 8192`). Component B (xHCI handoff) claims OS ownership via the USB Legacy Support
extended capability before resetting the controller; a no-op on QEMU (regression-verified), exercised
for real only on the Dell. Spec: `docs/superpowers/specs/2026-06-20-real-hw-boot-design.md`.

**Tech Stack:** C++ freestanding x86_64 kernel; `AddressSpace`/`MultibootMmap`/`FrameAllocator` are
host-tested (doctest, `make ARCH=x86_64 test`, ≥90% lcov); QEMU `qemu-system-x86_64 -m <big>` for the
memory smoke; xHCI MMIO ext-cap walk for the handoff.

**Conventions:** MI code reaches hardware only via `<arch/...>`; `make check-arch` must stay clean. Build
in Docker (`make image64`, `make ARCH=x86_64 test`), QEMU native. PTE flags live in
`arch/x86_64/mm/Paging.h`; `makeEntry(pa, flags)` keeps `flags & FLAG_MASK` (0xFFF, includes bit 7).

---

## File Structure (created/modified)

**Modified (MD, host-tested):**
- `arch/x86_64/mm/Paging.h` — add `PTE_PS` (bit 7, 2 MiB page).
- `arch/x86_64/mm/AddressSpace.h` / `.cpp` — add `mapRangeHuge`; make `translate()` huge-page aware.
- `arch/x86_64/boot/MultibootMmap.h` / `.cpp` — 64-bit highest-usable capped at 16 GiB.
- `mm/FrameAllocator.h` — expose the bitmap capacity (`capacityBytes()`) for the clamp.

**Modified (MD / wiring):**
- `arch/include/arch/bootinfo.h`, `arch/x86_64/boot/bootinfo_x86_64.cpp` — `bootMemTop()` → `uint64_t`.
- `arch/include/arch/mmu.h`, `arch/x86_64/mm/mmu_x86_64.cpp` — `mmuInitKernel(..., uint64_t topOfRam)`,
  identity-map via `mapRangeHuge`.
- `kernel/Kernel.cpp` — `markFree` callback passes 64-bit base/len; `g_frames.init` + `sysMemTotalKb`
  64-bit-clean.
- `arch/x86_64/drivers/xhci_x86_64.cpp` — `biosHandoff()` (USB Legacy Support) + guarded Intel port
  routing, called in `xhciInit` before `controllerInit`.

**New (verify):**
- `scripts/smoke-bigmem.sh` — boot `-m 6144` / `-m 8192`, assert shell + `MemTotal` > 4 GiB + no faults.

**Modified tests:** `tests/test_addressspace64.cpp`, `tests/test_multibootmmap.cpp`,
`tests/test_frameallocator.cpp`; `Makefile` (`smoke-bigmem`, `verify64`).

---

## Task 1: 2 MiB huge-page mapping in AddressSpace (MD, host-tested)

**Files:**
- Modify: `arch/x86_64/mm/Paging.h`, `arch/x86_64/mm/AddressSpace.h`, `arch/x86_64/mm/AddressSpace.cpp`
- Test: `tests/test_addressspace64.cpp`

- [ ] **Step 1: Add `PTE_PS` to `Paging.h`.** After the `PTE_PRIV` line:

```cpp
const uint64_t PTE_PS      = 1ULL << 7;                 // Page Size: this PD entry maps a 2 MiB page
```

- [ ] **Step 2: Declare `mapRangeHuge` in `AddressSpace.h`.** After the `mapRange` declaration (line ~29):

```cpp
	// Identity/normal map a 2 MiB-aligned range using 2 MiB huge pages (PD entries with PTE_PS).
	// `va`, `pa`, `len` are rounded to 2 MiB. Cheap for many-GiB ranges (one PD entry per 2 MiB).
	bool mapRangeHuge(uint64_t va, uint64_t pa, uint64_t len, uint64_t flags);
```

- [ ] **Step 3: Write the failing test** in `tests/test_addressspace64.cpp` (append a TEST_CASE). Maps a
  range at a HIGH va (5 GiB) and checks `translate()` resolves it, plus an offset within a 2 MiB page.

```cpp
TEST_CASE("mapRangeHuge maps 2 MiB pages and translate() resolves them") {
    auto env = makeTestEnv();                 // same helper the other AddressSpace cases use
    AddressSpace as(env);
    const uint64_t VA = 5ULL * 1024 * 1024 * 1024;   // 5 GiB (exercises the high range)
    REQUIRE(as.mapRangeHuge(VA, VA, 4ULL * 1024 * 1024, kernel::PTE_PRESENT | kernel::PTE_RW));
    CHECK(as.translate(VA) == VA);                       // start of the first 2 MiB page
    CHECK(as.translate(VA + 0x1FFFFF) == VA + 0x1FFFFF); // last byte of the first 2 MiB page
    CHECK(as.translate(VA + 0x200000) == VA + 0x200000); // second 2 MiB page
}
```

> Note: `makeTestEnv()` is whatever the existing cases in this file use to build a `PagingEnv` over a
> host frame pool. If the file names it differently, reuse that exact helper — do not invent a new one.

- [ ] **Step 4: Run it; expect FAIL** (`mapRangeHuge` undefined / `translate` returns ~0 for a huge page).

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "mapRangeHuge|addressspace|error"`
Expected: link/compile error or the translate CHECKs fail.

- [ ] **Step 5: Implement `mapRangeHuge` + huge-aware `translate` in `AddressSpace.cpp`.** Add after
  `map()` (line ~49):

```cpp
bool AddressSpace::mapRangeHuge(uint64_t va, uint64_t pa, uint64_t len, uint64_t flags) {
    if (!m_topPhys)
        return false;
    const uint64_t HP = 0x200000;                       // 2 MiB
    uint64_t pages = (len + HP - 1) / HP;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t v = va + i * HP, a = pa + i * HP;
        uint64_t* p4 = top();
        uint64_t* p3 = nextTable(p4, pml4Index(v), true); if (!p3) return false;
        uint64_t* p2 = nextTable(p3, pdptIndex(v), true); if (!p2) return false;
        p2[pdIndex(v)] = makeEntry(a, flags | PTE_PS);    // 2 MiB leaf at the PD level
    }
    return true;
}
```

  And replace the body of `translate()` (currently uses `walk`) with a manual descent that stops at a
  huge PD entry:

```cpp
uint64_t AddressSpace::translate(uint64_t va) const {
    AddressSpace* self = const_cast<AddressSpace*>(this);   // read-only walk; create=false
    uint64_t* p4 = self->top();
    uint64_t* p3 = self->nextTable(p4, pml4Index(va), false); if (!p3) return 0xFFFFFFFFFFFFFFFFULL;
    uint64_t* p2 = self->nextTable(p3, pdptIndex(va), false); if (!p2) return 0xFFFFFFFFFFFFFFFFULL;
    uint64_t pde = p2[pdIndex(va)];
    if (!entryPresent(pde)) return 0xFFFFFFFFFFFFFFFFULL;
    if (pde & PTE_PS) return entryAddr(pde) | (va & 0x1FFFFF);   // 2 MiB huge page
    uint64_t* p1 = self->nextTable(p2, pdIndex(va), false);   if (!p1) return 0xFFFFFFFFFFFFFFFFULL;
    uint64_t pte = p1[ptIndex(va)];
    if (!entryPresent(pte)) return 0xFFFFFFFFFFFFFFFFULL;
    return entryAddr(pte) | pageOffset(va);
}
```

- [ ] **Step 6: Run the test; expect PASS** (and the existing 4 KiB `translate` cases still pass).

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "test cases|addressspace|fail"`
Expected: all green, ≥90%.

- [ ] **Step 7: Commit.**

```bash
git add arch/x86_64/mm/Paging.h arch/x86_64/mm/AddressSpace.h arch/x86_64/mm/AddressSpace.cpp tests/test_addressspace64.cpp
git commit -m "feat(mm,x86_64): AddressSpace::mapRangeHuge (2 MiB pages) + huge-aware translate (host-tested)"
```

---

## Task 2: 64-bit boot memory top, capped at the frame-pool capacity (host-tested)

**Files:**
- Modify: `mm/FrameAllocator.h`, `arch/x86_64/boot/MultibootMmap.h`, `arch/x86_64/boot/MultibootMmap.cpp`
- Test: `tests/test_multibootmmap.cpp`, `tests/test_frameallocator.cpp`

- [ ] **Step 1: Expose the frame-pool capacity in `FrameAllocator.h`.** Add a public constant the
  boot-top clamp can reference (it must equal the private `MAX_PHYS`):

```cpp
	// Physical span the bitmap covers (so the boot layer clamps top-of-RAM to what we can track).
	static constexpr uint64_t CAPACITY_BYTES = 16ull * 1024 * 1024 * 1024;   // 16 GiB
```
  And change the private `MAX_PHYS` line to reference it (keep one source of truth):
```cpp
	static const uint64_t MAX_PHYS   = CAPACITY_BYTES;             // 16 GiB
```

- [ ] **Step 2: Write the failing test** in `tests/test_multibootmmap.cpp` (append). A memory map with a
  usable region above 4 GiB → highest-usable returns the real 64-bit top, capped at 16 GiB.

```cpp
TEST_CASE("highestUsable is 64-bit and capped at the frame-pool capacity") {
    // Two usable regions: low [0, 2 GiB) and high [4 GiB, 8 GiB). type 1 = usable.
    struct __attribute__((packed)) E { uint32_t size; uint64_t base; uint64_t len; uint32_t type; };
    static E ents[2] = {
        { sizeof(E) - 4, 0x0,          0x80000000ull, 1 },   // 0 .. 2 GiB
        { sizeof(E) - 4, 0x100000000ull, 0x100000000ull, 1 },// 4 GiB .. 8 GiB
    };
    uint64_t top = kernel::highestUsableInBuffer64(ents, sizeof ents);
    CHECK(top == 0x200000000ull);                            // 8 GiB, un-clamped (< 16 GiB cap)
}
TEST_CASE("highestUsable clamps above the 16 GiB pool capacity") {
    struct __attribute__((packed)) E { uint32_t size; uint64_t base; uint64_t len; uint32_t type; };
    static E ents[1] = { { sizeof(E) - 4, 0x0, 0x500000000ull, 1 } };   // 0 .. 20 GiB
    CHECK(kernel::highestUsableInBuffer64(ents, sizeof ents) == kernel::FrameAllocator::CAPACITY_BYTES);
}
```

- [ ] **Step 2b: Add a FrameAllocator high-range test** in `tests/test_frameallocator.cpp` (append) — a
  range based at 4 GiB is tracked at the right (high) frames, not wrapped to frame 0:

```cpp
TEST_CASE("FrameAllocator tracks a range based above 4 GiB without wrapping") {
    static kernel::FrameAllocator fa;
    fa.init(0x140000000ull);                 // 5 GiB top
    fa.markRangeFree(0x100000000ull, 0x40000000ull);   // free [4 GiB, 5 GiB)
    CHECK(fa.isUsed(0));                                 // frame 0 stays reserved (NOT wrongly freed)
    CHECK(!fa.isUsed(0x100000000ull / kernel::FRAME_SIZE));   // a 4 GiB frame is now free
}
```

- [ ] **Step 3: Run; expect FAIL** (`highestUsableInBuffer64` undefined).

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "highestUsable|multibootmmap|frameallocator|error"`

- [ ] **Step 4: Implement the 64-bit highest-usable.** In `MultibootMmap.h` declare:

```cpp
uint64_t highestUsableInBuffer64(const void* mmap, uint32_t len);
```
  In `MultibootMmap.cpp`, add it next to the existing 32-bit one (reuse the `accumulateTop`/`parseMmapBuffer`
  machinery, but cap at the frame pool instead of `clamp32`):

```cpp
#include "FrameAllocator.h"   // CAPACITY_BYTES
uint64_t highestUsableInBuffer64(const void* mmap, uint32_t len) {
    TopAcc acc{0};                      // the same accumulator struct accumulateTop fills
    parseMmapBuffer(mmap, len, &acc, accumulateTop);
    uint64_t cap = kernel::FrameAllocator::CAPACITY_BYTES;
    return acc.top > cap ? cap : acc.top;
}
```
  (If the accumulator type/name differs in this file, reuse the existing one — `accumulateTop` already
  writes a `uint64_t top`. Do not duplicate the parse loop.)

- [ ] **Step 5: Run; expect PASS.**

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "test cases|highestUsable|frameallocator|fail"`
Expected: all green, ≥90%.

- [ ] **Step 6: Commit.**

```bash
git add mm/FrameAllocator.h arch/x86_64/boot/MultibootMmap.h arch/x86_64/boot/MultibootMmap.cpp tests/test_multibootmmap.cpp tests/test_frameallocator.cpp
git commit -m "feat(mm): 64-bit highest-usable RAM capped at the 16 GiB frame-pool capacity (host-tested)"
```

---

## Task 3: Wire the 64-bit top + huge-page kernel map; QEMU big-RAM smoke

**Files:**
- Modify: `arch/include/arch/bootinfo.h`, `arch/x86_64/boot/bootinfo_x86_64.cpp`,
  `arch/include/arch/mmu.h`, `arch/x86_64/mm/mmu_x86_64.cpp`, `kernel/Kernel.cpp`
- Create: `scripts/smoke-bigmem.sh`; Modify: `Makefile`

> Verification = QEMU with big `-m`. The MI pieces were proven host-side in Tasks 1–2; this wires them.

- [ ] **Step 1: Widen `bootMemTop()` to `uint64_t`.**
  - `arch/include/arch/bootinfo.h`: change `uint32_t bootMemTop();` → `uint64_t bootMemTop();`.
  - `arch/x86_64/boot/bootinfo_x86_64.cpp`: change the impl to return `uint64_t` and call the 64-bit
    highest-usable:
```cpp
uint64_t bootMemTop() {
    const MultibootInfo* mbi = g_mbi;
    uint64_t top = mbi ? kernel::highestUsableAddr64(mbi) : 0;
    return top ? top : (256ull * 1024 * 1024);    // fallback 256 MiB if the bootloader gave no map
}
```
    (Add a `highestUsableAddr64(const MultibootInfo*)` next to the existing `highestUsableAddr` in the
    MultibootInfo helper — it forwards to `highestUsableInBuffer64` over the mmap, mirroring the 32-bit
    one. If `bootMemTop`'s current impl already routes through `highestUsableAddr`, just add the `64`
    variant and call it.)

- [ ] **Step 2: Widen `mmuInitKernel` + identity-map with huge pages.**
  - `arch/include/arch/mmu.h`: `void mmuInitKernel(kernel::FrameAllocator& fa, uint64_t topOfRam);`
  - `arch/x86_64/mm/mmu_x86_64.cpp`: change the signature to `uint64_t topOfRam` and replace the
    4 KiB identity map line `g_kspace->mapRange(0, 0, topOfRam, ...)` with a 2 MiB-rounded huge map:
```cpp
	uint64_t mapTop = (topOfRam + 0x1FFFFF) & ~0x1FFFFFull;     // round up to 2 MiB
	g_kspace->mapRangeHuge(0, 0, mapTop, kernel::PTE_PRESENT | kernel::PTE_RW);  // identity all RAM (2 MiB pages)
```

- [ ] **Step 3: 64-bit-clean the `Kernel.cpp` boot-memory wiring.**
  - The `markFree` callback (around line 57): drop the `uint32_t` casts so 64-bit ranges pass through:
```cpp
static void markFree(void* fa, uint64_t base, uint64_t len) {
	((FrameAllocator*) fa)->markRangeFree(base, len);
}
```
  - `g_frames.init(top)` (around line 220): `top` is now `uint64_t` (from `bootMemTop()`); ensure the
    local is `uint64_t`:
```cpp
	uint64_t top = arch::bootMemTop();
	g_frames.init(top);
	arch::bootMemForEachUsable(&g_frames, markFree);
```
  - `sysMemTotalKb()` (around line 51): keep it returning a KB count but compute in 64-bit:
```cpp
unsigned long sysMemTotalKb() { return (unsigned long) (arch::bootMemTop() / 1024ull); }
```
    (Update its forward declaration / the `/proc/meminfo` caller's type if it was `unsigned`.)

- [ ] **Step 4: `check-arch` + build.**

Run: `make check-arch && make image64 2>&1 | tail -3`
Expected: "OK: MI layer is arch-clean." and a built image.

- [ ] **Step 5: Write `scripts/smoke-bigmem.sh`** — boot with big RAM under BIOS (SeaBIOS, fast),
  assert shell + `MemTotal` > 4 GiB + zero faults.

```bash
#!/usr/bin/env bash
# smoke-bigmem.sh — boot the image with > 4 GiB RAM (6 GiB straddles the 3-4 GiB PCI hole, so a usable
# mmap region lives above 4 GiB) and assert the 64-bit memory map + huge-page kernel identity map work:
# shell reached, /proc/meminfo MemTotal > 4 GiB, zero faults. Catches the uint32 truncation/wrap.
set -u
IMG=disk/image64.img
SER=/tmp/nanos-bigmem-serial.log
INT=/tmp/nanos-bigmem-int.log
MON=/tmp/nanos-bigmem-qmon.sock
rm -f "$SER" "$INT" "$MON"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64' first"; exit 2; }
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -m 6144 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -serial file:"$SER" -monitor unix:"$MON",server,nowait -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 45); do grep -q "bash-5" "$SER" 2>/dev/null && break; sleep 1; done
# read /proc/meminfo through the shell via the monitor
python3 - "$MON" <<'PY'
import socket,time,sys
s=socket.socket(socket.AF_UNIX)
try: s.connect(sys.argv[1])
except: sys.exit(0)
time.sleep(0.3)
for c in "cat /proc/meminfo":
    k = 'spc' if c==' ' else c
    s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.05)
s.sendall(b"sendkey ret\n"); time.sleep(1)
s.close()
PY
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== big-RAM (6 GiB) boot smoke ==="
chk "bash-5"                                          "reached the shell with 6 GiB RAM"
# MemTotal in kB > 4 GiB (4194304 kB). awk picks the MemTotal line's number.
if awk '/MemTotal/{ if ($2+0 > 4194304) ok=1 } END{ exit ok?0:1 }' "$SER" 2>/dev/null; then
    echo "  OK  : /proc/meminfo MemTotal > 4 GiB (high RAM mapped + pooled)"; else
    echo "  FAIL: MemTotal not > 4 GiB"; grep -i MemTotal "$SER" | head -1 | sed 's/^/        /'; PASS=0; fi
no  "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault"     "no faults"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: triple fault in int log"; PASS=0; else echo "  OK  : no triple fault"; fi
echo "================================="
if [ "$PASS" = 1 ]; then echo "big-RAM boot smoke: PASS"; exit 0; else echo "big-RAM boot smoke: FAIL"; exit 1; fi
```

- [ ] **Step 6: Add `make smoke-bigmem` + wire `verify64`.** Add `smoke-bigmem` to the `.PHONY` smoke
  line, then:
```make
smoke-bigmem: image64
	bash scripts/smoke-bigmem.sh
```
  and extend `verify64`:
```make
verify64: test64 smoke-x86_64 smoke-uefi smoke-bigmem smoke-usb
	@echo "x86_64 verify: host tests + BIOS + UEFI + big-RAM + live-USB boot smokes all passed."
```

- [ ] **Step 7: Run it; expect PASS.**

Run: `chmod +x scripts/smoke-bigmem.sh && make smoke-bigmem`
Expected: shell reached with 6 GiB, MemTotal > 4 GiB, zero faults. (Also re-run `make smoke-x86_64` to
confirm the default `-m 512` path still boots.)

- [ ] **Step 8: Commit.**

```bash
git add arch/include/arch/bootinfo.h arch/x86_64/boot/bootinfo_x86_64.cpp arch/include/arch/mmu.h arch/x86_64/mm/mmu_x86_64.cpp kernel/Kernel.cpp scripts/smoke-bigmem.sh Makefile
git commit -m "feat(mm,x86_64): 64-bit top-of-RAM + huge-page kernel identity map; smoke-bigmem gate (>4 GiB)"
```

---

## Task 4: xHCI BIOS/firmware handoff (USB Legacy Support) + guarded Intel port routing

**Files:**
- Modify: `arch/x86_64/drivers/xhci_x86_64.cpp`

> QEMU has no Legacy Support cap and its xHCI vendor is not Intel, so both additions are no-ops there;
> verification = the existing USB smokes still PASS (regression). The real exercise is the Dell.

- [ ] **Step 1: Implement `biosHandoff()` + Intel port routing.** Add inside the anonymous namespace in
  `xhci_x86_64.cpp` (uses the existing `cap32`, `g_mmio`, `ioWaitSpin`, `Pci`, and the discovered
  `PciDevice`). Capability-register offset `CAP_HCCPARAMS1` already exists (0x10).

```cpp
// xHCI Extended Capability: USB Legacy Support (id 1). USBLEGSUP bit16=BIOS owned, bit24=OS owned;
// USBLEGCTLSTS (+4) holds SMI enables (low) + RW1C SMI status (high). Claim OS ownership, wait for
// BIOS to release (bounded), then silence the SMIs. No Legacy cap (e.g. QEMU) => no-op.
void biosHandoff() {
    uint32_t hcc1 = cap32(CAP_HCCPARAMS1);
    uint32_t xecp = (hcc1 >> 16) & 0xFFFF;            // xECP: dword offset from MMIO base
    if (!xecp) return;
    volatile uint32_t* cap = (volatile uint32_t*) (g_mmio + (uintptr_t)xecp * 4);
    for (int guard = 0; guard < 256; guard++) {
        uint32_t c = cap[0];
        uint8_t id = (uint8_t) (c & 0xFF);
        if (id == 1) {                                 // USB Legacy Support
            cap[0] = c | (1u << 24);                   // HC OS Owned Semaphore
            for (int i = 0; i < 1000000 && (cap[0] & (1u << 16)); i++) ioWaitSpin();
            uint32_t enableMask = (1u<<0)|(1u<<4)|(1u<<13)|(1u<<14)|(1u<<15);   // SMI enables
            uint32_t rw1cMask   = (1u<<20)|(1u<<29)|(1u<<30)|(1u<<31);          // SMI status (write 1 to clear)
            cap[1] = (cap[1] & ~enableMask) | rw1cMask;
            return;
        }
        uint8_t next = (uint8_t) ((c >> 8) & 0xFF);    // next: dword stride (0 = end)
        if (!next) return;
        cap = (volatile uint32_t*) ((volatile uint8_t*) cap + (uintptr_t)next * 4);
    }
}

// Intel chipsets (vendor 0x8086) share USB2 ports between EHCI and xHCI; route them to xHCI. Harmless
// on EHCI-less PCHs (Skylake+, incl. the Comet Lake target). Guarded so it never touches non-Intel HW.
void intelPortRoute(const PciDevice& d) {
    if (d.vendor != 0x8086) return;
    Pci::write32(d.bus, d.dev, d.func, 0xD8, 0xFFFFFFFFu);   // XUSB2PR: USB2 ports -> xHCI
    Pci::write32(d.bus, d.dev, d.func, 0xD0, 0xFFFFFFFFu);   // USB3_PSSEN: enable SuperSpeed
}
```

- [ ] **Step 2: Call them in `xhciInit` before `controllerInit()`.** After the MMIO base / `g_op`/`g_rt`/
  `g_db` are set and `Pci::enableBusMaster(d)` ran, and BEFORE `controllerInit();`:

```cpp
    biosHandoff();          // take the controller from BIOS/SMM (no-op on QEMU)
    intelPortRoute(d);      // route USB2 ports to xHCI on Intel (no-op elsewhere)
    controllerInit();
```

- [ ] **Step 3: Build + arch-clean.**

Run: `make check-arch && make image64 2>&1 | tail -2`
Expected: arch-clean (xHCI is MD), image builds.

- [ ] **Step 4: QEMU regression — USB still works (handoff is a no-op here).**

Run: `make smoke-usb`
Expected: PASS — root mounts over USB-MSC, shell reached, zero faults (proves the handoff/route code
doesn't break the working QEMU path). Also `make smoke-x86_64` PASS.

- [ ] **Step 5: Commit.**

```bash
git add arch/x86_64/drivers/xhci_x86_64.cpp
git commit -m "feat(usb,x86_64): xHCI BIOS/SMM handoff (USB Legacy Support) + guarded Intel port routing (real-HW; no-op on QEMU)"
```

---

## Notes for the implementer

- **Tasks 1–2 are pure MI/MD-host TDD** — the truncation/wrap bug and the huge-page map are proven on
  the host before any QEMU. Task 3 wires them and the `-m 6144` smoke is the integration proof. Task 4
  (handoff) cannot be proven in QEMU (no SMM owner) — its CI bar is "USB smokes still green"; the real
  validation is the Dell.
- **Huge-page vs privatization:** the kernel identity map (low VAs, identity) and the user windows
  (`VA_USER_BASE` … `VA_MMAP_MAX`, all < ~1.4 GiB) live in different PD entries; `mapRangeHuge` only
  writes kernel-half PD leaves. After Task 3, re-run the full host suite — `test_addressspace64`'s
  fork/adopt/privatize cases must stay green (they exercise `adoptKernelDirectory` sharing the kernel
  half). If any regress, the huge PD entries leaked into a user-window PD — they must not.
- **`translate()` rewrite (Task 1)** must keep the existing 4 KiB cases passing — it now descends
  manually and special-cases `PTE_PS`; verify no other caller depended on `walk()` semantics it changed
  (it didn't; `translate` is the only consumer touched).
- **MemTotal assertion:** if `/proc/meminfo` rounds or names the field differently, adjust the awk in
  `smoke-bigmem.sh`; the substance is "a value > 4194304 kB appears," proving RAM above 4 GiB is counted.
- **Out of scope:** NVMe, e1000e/I219, APIC, Secure Boot, ext4-root-under-Limine.
```
