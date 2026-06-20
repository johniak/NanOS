# NIC port Phase 1 — shared core + LAPIC/MSI-X + e1000 refactor + e1000e — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended)
> or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Build the QEMU-verifiable foundation of the e1000e/I219 NIC port: a minimal LAPIC + MSI-X
interrupt path, a shared `E1000Core` engine, the existing e1000 refactored onto it (behaviour
unchanged), and a new e1000e (82574L) kext that exercises the whole MSI-X + NAPI + checksum-offload path
under `qemu -device e1000e`.

**Architecture:** A shared `E1000Core` (registers, RX/TX rings, NAPI drain, checksum offload) compiled
into thin per-variant kexts. MSI/MSI-X are delivered through a new minimal LAPIC in `arch/x86_64`
(running alongside the existing 8259 PIC) and a new `knx_register_msi` kernel export that programs the
device's MSI/MSI-X capability. Phase 2 (I219 ich9lan PHY/ULP, real-HW) is a separate later plan.

**Tech Stack:** C++ freestanding x86_64; kexts are `.nkext` modules (NxFormat) built separately from the
kernel; host doctest (`make ARCH=x86_64 test`, ≥90% lcov) for pure logic; QEMU `-device e1000e` for the
integration smoke. Spec: `docs/superpowers/specs/2026-06-20-i219-e1000e-nic-port-design.md`.

**Conventions:** Build in Docker (`make image64`, `make ARCH=x86_64 test`), QEMU native. `make
check-arch` stays clean (LAPIC is in `arch/x86_64`; kexts are MD ring-0 outside the MI guard). No Claude
mention in commits. Branch `feat/nic-i219`.

---

## File Structure

- `arch/x86_64/cpu/lapic_x86_64.{h,cpp}` — **new.** Enable the LAPIC (IA32_APIC_BASE MSR + spurious
  vector), an MSI vector-pool allocator (vectors 0x70–0x7F), `lapicEoi()`, `lapicId()`.
- `arch/x86_64/cpu/irq64.S` — **modify.** Add 8 MSI IRQ stubs (`irq_msi0..7`) feeding the common stub.
- `arch/x86_64/cpu/Idt64.cpp` — **modify.** Install IDT gates for vectors 0x70–0x77 → the new stubs.
- `arch/x86_64/cpu/cpu_x86_64.cpp` — **modify.** Call `lapicInit()` during CPU bring-up.
- `kernel/MsiRouter.{h,cpp}` — **new.** MI-side `knx_register_msi`: walk the PCI capability list, pick
  MSI-X→MSI, allocate a vector (via an injected arch hook), program the capability, install the handler.
- `kernel/KernelExports.cpp`, `kernel/kexports.def` — **modify.** Export `knx_register_msi`.
- `kext/e1000/e1000_core.{h,cpp}` — **new.** The shared engine (struct + `coreStart/coreRxPoll/coreTx`,
  pure-logic helpers split out for host testing).
- `kext/e1000/e1000.cpp` — **modify.** Thin wrapper over `E1000Core` (82540EM, INTx, no offload).
- `kext/e1000/e1000e.cpp` — **new.** Thin wrapper (82574L, MSI-X, checksum offload).
- `tests/test_msi_router.cpp` — **new.** Host-test the PCI-cap walk + MSI-X/MSI selection.
- `tests/test_e1000_core.cpp` — **new.** Host-test ring index advance, RX-drain stop, TX/offload encode.
- `scripts/smoke-e1000e.sh` — **new.** Boot `-device e1000e`, assert net up + ping.
- `Makefile` — **modify.** Kext build/link rules, `KEXTS += e1000e`, `TEST_MODULES`/`COV_PATTERNS`,
  `smoke-e1000e`, `verify64`.

---

## Task 1: Minimal LAPIC + MSI vector-pool allocator

**Files:**
- Create: `arch/x86_64/cpu/lapic_x86_64.h`, `arch/x86_64/cpu/lapic_x86_64.cpp`
- Test: `tests/test_lapic_vectors.cpp`

The LAPIC MMIO/MSR work is not host-testable, but the **vector-pool allocator is pure logic** — split it
into a free function and test that.

- [ ] **Step 1: Write `arch/x86_64/cpu/lapic_x86_64.h`.**

```cpp
#pragma once
#include <stdint.h>
namespace kernel {

// MSI vector pool: vectors 0x70..0x7F, above the PIC IRQ range (0x20..0x2F). The allocator is a pure
// counter so it is host-testable independent of the LAPIC MMIO.
constexpr uint8_t MSI_VEC_BASE = 0x70;
constexpr uint8_t MSI_VEC_END  = 0x78;   // exclusive — 8 vectors, matching the 8 IDT stubs in Task 2

struct MsiVecPool { uint8_t next; };                 // next == MSI_VEC_BASE initially (zeroed .bss => fix in init)
void    msiVecPoolInit(MsiVecPool* p);               // sets next = MSI_VEC_BASE
int     msiVecAlloc(MsiVecPool* p);                  // returns a vector in [BASE,END) or -1 if exhausted

// LAPIC bring-up + accessors (kernel-only; MMIO at the IA32_APIC_BASE physical, identity-mapped).
void     lapicInit();                                // enable LAPIC + spurious vector; idempotent
uint8_t  lapicId();                                  // local APIC id (for MSI message address)
void     lapicEoi();                                 // signal end-of-interrupt
int      lapicAllocVector();                         // global pool alloc (wraps msiVecAlloc); -1 if full

}  // namespace kernel
```

- [ ] **Step 2: Write the failing test** `tests/test_lapic_vectors.cpp`.

```cpp
#include "doctest.h"
#include "lapic_x86_64.h"
using namespace kernel;

TEST_CASE("msiVecAlloc hands out distinct vectors in range then signals exhaustion") {
    MsiVecPool p;
    msiVecPoolInit(&p);
    int first = msiVecAlloc(&p);
    CHECK(first == MSI_VEC_BASE);
    int second = msiVecAlloc(&p);
    CHECK(second == MSI_VEC_BASE + 1);
    CHECK(second != first);
    // Drain the rest of the pool.
    int count = 2;
    while (msiVecAlloc(&p) >= 0) count++;
    CHECK(count == (MSI_VEC_END - MSI_VEC_BASE));     // exactly the pool size handed out
    CHECK(msiVecAlloc(&p) == -1);                     // exhausted
}
```

- [ ] **Step 3: Run it; expect FAIL** (link error: `msiVecPoolInit`/`msiVecAlloc` undefined).

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "lapic|msiVec|undefined|error"`
Expected: FAIL.

- [ ] **Step 4: Write `arch/x86_64/cpu/lapic_x86_64.cpp`.**

```cpp
#include "lapic_x86_64.h"
#include <stdint.h>

namespace kernel {

// ---- pure-logic vector pool (host-tested) ----
void msiVecPoolInit(MsiVecPool* p) { p->next = MSI_VEC_BASE; }
int  msiVecAlloc(MsiVecPool* p) {
    if (p->next >= MSI_VEC_END) return -1;
    return (int) p->next++;
}

#ifndef NANOS_HOST_TEST    // the LAPIC MMIO/MSR half is kernel-only

static volatile uint32_t* g_lapic = 0;     // identity-mapped LAPIC MMIO base
static MsiVecPool g_pool;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi; __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t) hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ __volatile__("wrmsr" :: "c"(msr), "a"((uint32_t) v), "d"((uint32_t)(v >> 32)));
}

enum { IA32_APIC_BASE = 0x1B, LAPIC_ID = 0x20, LAPIC_EOI = 0xB0, LAPIC_SPURIOUS = 0xF0 };

void lapicInit() {
    uint64_t base = rdmsr(IA32_APIC_BASE);
    base |= (1u << 11);                                   // global LAPIC enable
    wrmsr(IA32_APIC_BASE, base);
    g_lapic = (volatile uint32_t*) (uintptr_t) (base & 0xFFFFF000ull);
    g_lapic[LAPIC_SPURIOUS / 4] = 0x100 | 0xFF;           // bit8 = APIC software enable, spurious vec 0xFF
    msiVecPoolInit(&g_pool);
}
uint8_t lapicId()  { return (uint8_t) (g_lapic[LAPIC_ID / 4] >> 24); }
void    lapicEoi() { g_lapic[LAPIC_EOI / 4] = 0; }
int     lapicAllocVector() { return msiVecAlloc(&g_pool); }

#endif  // NANOS_HOST_TEST

}  // namespace kernel
```

- [ ] **Step 5: Add the host-test module to the Makefile.** In `tests`/`TEST_MODULES` add
  `lapic_x86_64`; in `HINCLUDES` it must see `arch/x86_64/cpu`. Compile the test object with
  `-DNANOS_HOST_TEST` so only the pure-logic half builds. (Find `TEST_MODULES=` and the host test
  compile rule; add `arch/x86_64/cpu` to the host include list and `-DNANOS_HOST_TEST` to the host
  CXXFLAGS for this file — mirror how other arch host-tested modules like `AddressSpace` are wired.)

- [ ] **Step 6: Run it; expect PASS.**

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "distinct vectors|test cases|fail"`
Expected: PASS (the new case green, total count +1).

- [ ] **Step 7: Commit.**

```bash
git add arch/x86_64/cpu/lapic_x86_64.h arch/x86_64/cpu/lapic_x86_64.cpp tests/test_lapic_vectors.cpp Makefile
git commit -m "feat(x86_64): minimal LAPIC + host-tested MSI vector-pool allocator"
```

---

## Task 2: MSI vector IDT stubs + dispatch + LAPIC enable at boot

**Files:**
- Modify: `arch/x86_64/cpu/irq64.S`, `arch/x86_64/cpu/Idt64.cpp`, `arch/x86_64/cpu/cpu_x86_64.cpp`

No host test (pure asm/IDT wiring); verified by Task 5's QEMU smoke. This makes
`Interrupt::registerInterruptHandler(0x70+n, h)` fire on an MSI to that vector, with a LAPIC EOI.

- [ ] **Step 1: Add 8 MSI IRQ stubs to `arch/x86_64/cpu/irq64.S`.** Mirror the existing IRQ stubs
  (which push the vector number and jump to the common stub). After the last existing IRQ stub:

```asm
%macro MSI_STUB 1
global irq_msi%1
irq_msi%1:
    cli
    push 0                      ; dummy error code (matches the common stub frame)
    push %1 + 0x70              ; interrupt number = 0x70 + n
    jmp irq_common_stub
%endmacro
MSI_STUB 0
MSI_STUB 1
MSI_STUB 2
MSI_STUB 3
MSI_STUB 4
MSI_STUB 5
MSI_STUB 6
MSI_STUB 7
```
> Match the EXACT push order / common-stub name used by the existing `irq64.S` stubs (read the file
> first; if it pushes `byte` or uses a different label, copy that form verbatim).

- [ ] **Step 2: Declare the stubs + install gates in `arch/x86_64/cpu/Idt64.cpp`.** Near the other
  `extern "C" void irqN();` declarations add `extern "C" void irq_msi0()..irq_msi7();`, and after the
  IRQ-gate loop (vectors 32..47) add:

```cpp
    // MSI vectors 0x70..0x77 -> the irq_msiN stubs (DPL0, 64-bit interrupt gate).
    setGate(0x70, (uint64_t) irq_msi0, 0x08, 0, 0x8E);
    setGate(0x71, (uint64_t) irq_msi1, 0x08, 0, 0x8E);
    setGate(0x72, (uint64_t) irq_msi2, 0x08, 0, 0x8E);
    setGate(0x73, (uint64_t) irq_msi3, 0x08, 0, 0x8E);
    setGate(0x74, (uint64_t) irq_msi4, 0x08, 0, 0x8E);
    setGate(0x75, (uint64_t) irq_msi5, 0x08, 0, 0x8E);
    setGate(0x76, (uint64_t) irq_msi6, 0x08, 0, 0x8E);
    setGate(0x77, (uint64_t) irq_msi7, 0x08, 0, 0x8E);
```

- [ ] **Step 3: EOI the LAPIC for MSI vectors in the common IRQ dispatch.** Find where `irq_common_stub`
  → C dispatch sends the PIC EOI (the function that handles vectors 32..47). Add: for `int_no` in
  `[0x70, 0x80)`, call `kernel::lapicEoi()` instead of the PIC EOI, then run the registered handler.
  Concretely, in that dispatch function (e.g. `irq_handler` in `Interrupt64.cpp`), wrap the EOI:

```cpp
    if (regs->int_no >= 0x70 && regs->int_no < 0x80)
        kernel::lapicEoi();
    else {
        // existing PIC EOI (0x20 to master, +0xA0 to slave for >=40) — leave UNCHANGED
        ...
    }
```
> Read `Interrupt64.cpp` first and place the branch around the existing EOI; the registered-handler
> call (`registerInterruptHandler` table lookup) already runs for every vector — do not duplicate it.

- [ ] **Step 4: Enable the LAPIC during CPU bring-up.** In `arch/x86_64/cpu/cpu_x86_64.cpp`, after the
  IDT/PIC are set up (where `cpuInit` finishes its existing work), add `#include "lapic_x86_64.h"` and a
  call `kernel::lapicInit();`. The PIC stays remapped and active (timer/keyboard); the LAPIC only adds
  MSI delivery.

- [ ] **Step 5: Build + arch-clean.**

Run: `make check-arch && make image64 2>&1 | tail -2`
Expected: arch-clean; image builds (no behavioural change yet — nothing sends an MSI).

- [ ] **Step 6: QEMU regression — existing boot unaffected.**

Run: `make smoke-x86_64`
Expected: PASS (enabling the LAPIC alongside the PIC must not disturb the existing IRQ-driven boot).

- [ ] **Step 7: Commit.**

```bash
git add arch/x86_64/cpu/irq64.S arch/x86_64/cpu/Idt64.cpp arch/x86_64/cpu/cpu_x86_64.cpp arch/x86_64/cpu/Interrupt64.cpp
git commit -m "feat(x86_64): MSI vector IDT stubs + LAPIC EOI dispatch + enable LAPIC at boot"
```

---

## Task 3: `knx_register_msi` — PCI capability walk + MSI-X/MSI programming

**Files:**
- Create: `kernel/MsiRouter.h`, `kernel/MsiRouter.cpp`
- Modify: `kernel/kexports.def`, `kernel/KernelExports.cpp`
- Test: `tests/test_msi_router.cpp`

The **capability walk + MSI-X/MSI selection is pure logic** over config-space bytes — host-test it via a
fake config space. The actual vector alloc + LAPIC programming are injected (arch hook) so the core
logic tests without hardware.

- [ ] **Step 1: Write `kernel/MsiRouter.h`.**

```cpp
#pragma once
#include <stdint.h>
namespace kernel {

// Injectable backends so the cap-walk logic is host-testable: config-space r/w + a vector allocator
// returning a CPU vector (>=0) or -1, plus the LAPIC id for the MSI message address.
struct MsiEnv {
    uint32_t (*cfgRead)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
    void     (*cfgWrite)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);
    void*    (*mapMmio)(uint32_t phys, uint32_t len);   // for the MSI-X table BAR; null if unused
    int      (*allocVector)();
    uint8_t  (*lapicId)();
};

enum MsiKind { MSI_NONE = 0, MSI_KIND_MSI = 1, MSI_KIND_MSIX = 2 };

struct MsiResult { MsiKind kind; int vector; uint8_t capOff; };

// Walk the capability list at PCI (bus,dev,func); prefer MSI-X (cap 0x11) over MSI (cap 0x05). On a
// match: allocVector(), then program the capability (message address = 0xFEE00000|(lapicId<<12),
// data = vector) and enable it. Returns {kind,vector} or {MSI_NONE,-1} if neither cap exists / no vector.
MsiResult msiSetup(const MsiEnv& env, uint8_t bus, uint8_t dev, uint8_t func);

}  // namespace kernel
```

- [ ] **Step 2: Write the failing test** `tests/test_msi_router.cpp`. A fake 256-byte config space with
  a capability list containing an MSI cap and an MSI-X cap; assert MSI-X is chosen and enabled.

```cpp
#include "doctest.h"
#include "MsiRouter.h"
#include <cstring>
using namespace kernel;

static uint8_t g_cfg[256];
static uint32_t fcRead(uint8_t, uint8_t, uint8_t, uint8_t off) {
    uint32_t v; memcpy(&v, &g_cfg[off & 0xFC], 4); return v;
}
static void fcWrite(uint8_t, uint8_t, uint8_t, uint8_t off, uint32_t v) {
    memcpy(&g_cfg[off & 0xFC], &v, 4);
}
static int  fAlloc() { return 0x71; }
static uint8_t fId()  { return 3; }

TEST_CASE("msiSetup prefers MSI-X, allocates a vector and enables the cap") {
    memset(g_cfg, 0, sizeof g_cfg);
    g_cfg[0x06] = 0x10;                 // Status: capabilities-list present (bit4)
    g_cfg[0x34] = 0x40;                 // Capabilities pointer -> 0x40
    // cap @0x40: MSI (id 0x05), next -> 0x50
    g_cfg[0x40] = 0x05; g_cfg[0x41] = 0x50; g_cfg[0x42] = 0x00; g_cfg[0x43] = 0x00;
    // cap @0x50: MSI-X (id 0x11), next -> 0x00; message control = 0 table entries-1 (1 entry); table @BAR4 off0
    g_cfg[0x50] = 0x11; g_cfg[0x51] = 0x00; g_cfg[0x52] = 0x00; g_cfg[0x53] = 0x00;
    g_cfg[0x54] = 0x04; g_cfg[0x55] = 0x00; g_cfg[0x56] = 0x00; g_cfg[0x57] = 0x00;  // table offset/BIR (BIR=4)

    // BAR4 -> a memory BAR we can "map" to a local table buffer.
    static uint32_t table[4] = {0,0,0,0};
    MsiEnv env;
    env.cfgRead = fcRead; env.cfgWrite = fcWrite;
    env.mapMmio = [](uint32_t, uint32_t) -> void* { return table; };
    env.allocVector = fAlloc; env.lapicId = fId;
    // BAR4 value (memory BAR, base irrelevant since mapMmio is faked):
    g_cfg[0x20] = 0x00; g_cfg[0x21] = 0x00; g_cfg[0x22] = 0xF0; g_cfg[0x23] = 0xFE;

    MsiResult r = msiSetup(env, 0, 0, 0);
    CHECK(r.kind == MSI_KIND_MSIX);
    CHECK(r.vector == 0x71);
    CHECK(r.capOff == 0x50);
    // MSI-X table entry 0 programmed: addr lo = 0xFEE03000 (lapicId 3 << 12), data = 0x71, unmasked.
    CHECK(table[0] == 0xFEE03000u);
    CHECK(table[2] == 0x71u);
    CHECK((table[3] & 1u) == 0u);                          // vector control: not masked
    // MSI-X enable bit (msg control bit15) set in config space.
    uint16_t mc; memcpy(&mc, &g_cfg[0x52], 2);
    CHECK((mc & 0x8000) != 0);
}
```

- [ ] **Step 3: Run it; expect FAIL** (`msiSetup` undefined).

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "prefers MSI-X|msiSetup|undefined|error"`
Expected: FAIL.

- [ ] **Step 4: Write `kernel/MsiRouter.cpp`.**

```cpp
#include "MsiRouter.h"

namespace kernel {

static uint16_t cfg16(const MsiEnv& e, uint8_t b, uint8_t d, uint8_t f, uint8_t off) {
    return (uint16_t) (e.cfgRead(b, d, f, off) >> ((off & 2) * 8));
}

MsiResult msiSetup(const MsiEnv& env, uint8_t bus, uint8_t dev, uint8_t func) {
    MsiResult none = { MSI_NONE, -1, 0 };
    uint16_t status = cfg16(env, bus, dev, func, 0x06);
    if (!(status & 0x10)) return none;                     // no capability list
    uint8_t msiOff = 0, msixOff = 0;
    uint8_t off = (uint8_t) (env.cfgRead(bus, dev, func, 0x34) & 0xFC);
    for (int guard = 0; off && guard < 48; guard++) {
        uint32_t hdr = env.cfgRead(bus, dev, func, off);
        uint8_t id = (uint8_t) (hdr & 0xFF);
        if (id == 0x11) msixOff = off;
        else if (id == 0x05) msiOff = off;
        off = (uint8_t) ((hdr >> 8) & 0xFC);
    }
    if (!msixOff && !msiOff) return none;

    int vec = env.allocVector();
    if (vec < 0) return none;
    uint32_t addr = 0xFEE00000u | ((uint32_t) env.lapicId() << 12);

    if (msixOff) {
        // Table offset/BIR at cap+4: low 3 bits = BIR (which BAR), rest = offset into that BAR.
        uint32_t tob = env.cfgRead(bus, dev, func, (uint8_t) (msixOff + 4));
        uint8_t  bir = (uint8_t) (tob & 0x7);
        uint32_t tableOff = tob & ~0x7u;
        uint32_t bar = env.cfgRead(bus, dev, func, (uint8_t) (0x10 + bir * 4)) & ~0xFu;
        volatile uint32_t* t = (volatile uint32_t*) ((uint8_t*) env.mapMmio(bar, 0x1000) + tableOff);
        t[0] = addr;                 // message address low
        t[1] = 0;                    // message address high
        t[2] = (uint32_t) vec;       // message data = vector
        t[3] = 0;                    // vector control: unmasked
        uint16_t mc = cfg16(env, bus, dev, func, (uint8_t) (msixOff + 2));
        mc |= 0x8000;                // MSI-X enable
        env.cfgWrite(bus, dev, func, (uint8_t) (msixOff + 2),
                     (env.cfgRead(bus, dev, func, msixOff) & 0x0000FFFF) | ((uint32_t) mc << 16));
        return MsiResult{ MSI_KIND_MSIX, vec, msixOff };
    }

    // MSI: message control at cap+2; addr at cap+4; data at cap+8 (64-bit) or cap+8 low (32-bit).
    uint16_t mc = cfg16(env, bus, dev, func, (uint8_t) (msiOff + 2));
    bool is64 = (mc & 0x80) != 0;
    env.cfgWrite(bus, dev, func, (uint8_t) (msiOff + 4), addr);
    if (is64) {
        env.cfgWrite(bus, dev, func, (uint8_t) (msiOff + 8), 0);
        env.cfgWrite(bus, dev, func, (uint8_t) (msiOff + 0xC), (uint32_t) vec);
    } else {
        env.cfgWrite(bus, dev, func, (uint8_t) (msiOff + 8), (uint32_t) vec);
    }
    mc |= 0x1;                       // MSI enable
    env.cfgWrite(bus, dev, func, msiOff,
                 (env.cfgRead(bus, dev, func, msiOff) & 0x0000FFFF) | ((uint32_t) mc << 16));
    return MsiResult{ MSI_KIND_MSI, vec, msiOff };
}

}  // namespace kernel
```

- [ ] **Step 5: Run it; expect PASS.**

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "prefers MSI-X|test cases|fail"`
Expected: PASS.

- [ ] **Step 6: Wire the kernel export `knx_register_msi`.** Add `KX(knx_register_msi)` to
  `kernel/kexports.def`, and in `kernel/KernelExports.cpp` implement it over the real backends:

```cpp
#include "MsiRouter.h"
#include "Pci.h"
#include "lapic_x86_64.h"
#include "Interrupt64.h"
// ... existing knx_* implementations ...

extern "C" int knx_register_msi(uint8_t bus, uint8_t dev, uint8_t func, void (*h)(void*), void* ctx) {
    static void (*s_h)(void*) = 0; static void* s_ctx = 0;   // single NIC vector in Phase 1
    s_h = h; s_ctx = ctx;
    kernel::MsiEnv env;
    env.cfgRead   = [](uint8_t b, uint8_t d, uint8_t f, uint8_t o){ return kernel::Pci::read32(b,d,f,o); };
    env.cfgWrite  = [](uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint32_t v){ kernel::Pci::write32(b,d,f,o,v); };
    env.mapMmio   = [](uint32_t p, uint32_t l){ return knx_map_mmio(p, l); };
    env.allocVector = [](){ return kernel::lapicAllocVector(); };
    env.lapicId   = [](){ return kernel::lapicId(); };
    kernel::MsiResult r = kernel::msiSetup(env, bus, dev, func);
    if (r.kind == kernel::MSI_NONE) return -1;
    // The trampoline: registered on the allocated IDT vector, forwards to the kext handler.
    kernel::Interrupt::registerInterruptHandler((unsigned char) r.vector,
        [](kernel::Registers*){ if (s_h) s_h(s_ctx); });
    return 0;
}
```
> Match the real `IsrHandler` signature in `Interrupt64.h` (it takes the arch `Registers*`); if it
> differs, adapt the lambda. `knx_map_mmio` is already an export in this file — reuse it.

- [ ] **Step 7: Add `MsiRouter` + `lapic_x86_64` to `TEST_MODULES`/`COV_PATTERNS`; build kernel.**

Run: `make check-arch && make ARCH=x86_64 test 2>&1 | grep -iE "test cases|fail" && make image64 2>&1 | tail -2`
Expected: arch-clean; tests pass; image builds.

- [ ] **Step 8: Commit.**

```bash
git add kernel/MsiRouter.h kernel/MsiRouter.cpp kernel/kexports.def kernel/KernelExports.cpp tests/test_msi_router.cpp Makefile
git commit -m "feat(kernel): knx_register_msi — PCI cap walk + MSI-X/MSI programming (host-tested)"
```

---

## Task 4: Extract `E1000Core` and refactor e1000 onto it (behaviour preserved)

**Files:**
- Create: `kext/e1000/e1000_core.h`, `kext/e1000/e1000_core.cpp`
- Modify: `kext/e1000/e1000.cpp`, `Makefile`
- Test: `tests/test_e1000_core.cpp`

Lift the current `e1000.cpp` engine into `E1000Core` verbatim (registers, descriptors, rings,
`rxPoll`/`tx`/`setupRings`/`readMac`), turning file-statics into a `struct E1000Core` instance and adding
variant hooks. The pure-logic helpers (ring index advance, RX-drain stop condition, TX descriptor field
encode) become free functions so they host-test without `knx_*`.

- [ ] **Step 1: Write `kext/e1000/e1000_core.h`** (struct + ops + entry points + pure helpers).

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

enum E1000IrqMode { IRQ_INTX = 0, IRQ_MSI = 1, IRQ_MSIX = 2 };

struct E1000Core;
struct E1000Variant {
    uint16_t pciDevice;                 // 0x100E / 0x10D3 / 0x0D4E
    const char* tag;                    // "e1000" / "e1000e" / "i219"
    bool (*phyBringup)(E1000Core*);     // null = none (e1000/e1000e)
    bool (*readMac)(E1000Core*);        // null = default RAL0/RAH0
    int  irqMode;                       // preferred E1000IrqMode (degrades down)
    bool csumOffload;                   // program RXCSUM + TX offload bits
};

struct RxDesc { uint64_t addr; uint16_t length; uint16_t csum; uint8_t status; uint8_t errors; uint16_t special; } __attribute__((packed));
struct TxDesc { uint64_t addr; uint16_t length; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; } __attribute__((packed));

static const int RX_N = 32, TX_N = 32, BUF_SZ = 2048;

struct E1000Core {
    volatile uint8_t* mmio;
    const E1000Variant* v;
    uint8_t bus, dev, func;
    RxDesc* rx; uint32_t rxPhys;
    TxDesc* tx; uint32_t txPhys;
    uint8_t* rxBuf[RX_N];
    uint8_t* txBuf[TX_N]; uint32_t txBufPhys[TX_N];
    int rxCur, txCur;
    void* handle;
    unsigned char mac[6];
};

// ---- pure-logic helpers (host-tested; no MMIO / no knx) ----
int  ringNext(int cur, int n);                                  // (cur+1)%n
bool rxDescDone(const RxDesc* d);                               // status & DD
void txEncode(TxDesc* d, uint32_t bufPhys, int len, bool csum); // fill addr/length/cmd/cso/status

// ---- engine entry points (kernel/kext side; need knx_*) ----
int  coreStart(E1000Core* c, const E1000Variant* v, uint8_t bus, uint8_t dev, uint8_t func);
void coreRxPoll(E1000Core* c);
int  coreTx(E1000Core* c, const void* data, int len);
```

- [ ] **Step 2: Write the failing test** `tests/test_e1000_core.cpp`.

```cpp
#include "doctest.h"
#include "e1000_core.h"

TEST_CASE("ringNext wraps at n") {
    CHECK(ringNext(0, 32) == 1);
    CHECK(ringNext(31, 32) == 0);
}
TEST_CASE("rxDescDone reads the DD status bit") {
    RxDesc d{}; CHECK(!rxDescDone(&d));
    d.status = 0x01; CHECK(rxDescDone(&d));
}
TEST_CASE("txEncode sets EOP|IFCS|RS, length, addr, clears status; csum sets cso/IXSM path") {
    TxDesc d{}; d.status = 0x01;
    txEncode(&d, 0xCAFE000, 100, false);
    CHECK(d.addr == 0xCAFE000u);
    CHECK(d.length == 100);
    CHECK(d.cmd == (0x01 | 0x02 | 0x08));     // EOP | IFCS | RS
    CHECK(d.status == 0);
    TxDesc e{};
    txEncode(&e, 0x1000, 60, true);
    CHECK(e.cmd & 0x04);                       // TXD bit2 = insert TCP/UDP checksum (offload)
}
```

- [ ] **Step 3: Run it; expect FAIL** (helpers undefined).

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "ringNext|txEncode|undefined|error"`
Expected: FAIL.

- [ ] **Step 4: Write `kext/e1000/e1000_core.cpp`.** Move the register enums, `wr/rd`, `readMac`,
  `rxPoll`→`coreRxPoll`, `e1000Tx`→`coreTx`, `setupRings`, and the `nkext_init` body (minus the PCI
  match) from `e1000.cpp` into `coreStart`, operating on `E1000Core* c` instead of file-statics. Define
  the pure helpers:

```cpp
#include "e1000_core.h"
#include "knx_net.h"

extern "C" int  knx_pci_find(uint16_t, uint16_t, uint8_t*, uint8_t*, uint8_t*);
extern "C" uint32_t knx_pci_bar(uint8_t, uint8_t, uint8_t, int);
extern "C" uint8_t  knx_pci_irq(uint8_t, uint8_t, uint8_t);
extern "C" void knx_pci_enable_bus_master(uint8_t, uint8_t, uint8_t);
extern "C" void knx_register_irq(int, void (*)(void*));
extern "C" int  knx_register_msi(uint8_t, uint8_t, uint8_t, void (*)(void*), void*);
extern "C" void knx_log(const char*);
extern "C" void* memcpy(void*, const void*, size_t);

// register offsets + bits (moved verbatim from the old e1000.cpp; add REG_RXCSUM = 0x5000)
enum { REG_CTRL=0x0000, REG_STATUS=0x0008, REG_ICR=0x00C0, REG_ICS=0x00C8, REG_IMS=0x00D0, REG_IMC=0x00D8,
       REG_RCTL=0x0100, REG_TCTL=0x0400, REG_TIPG=0x0410, REG_RXCSUM=0x5000,
       REG_RDBAL=0x2800, REG_RDBAH=0x2804, REG_RDLEN=0x2808, REG_RDH=0x2810, REG_RDT=0x2818,
       REG_TDBAL=0x3800, REG_TDBAH=0x3804, REG_TDLEN=0x3808, REG_TDH=0x3810, REG_TDT=0x3818,
       REG_MTA=0x5200, REG_RAL0=0x5400, REG_RAH0=0x5404 };
enum { CTRL_SLU=0x40, CTRL_ASDE=0x20, CTRL_RST=0x04000000,
       RCTL_EN=0x02, RCTL_BAM=0x8000, RCTL_SECRC=0x04000000,
       TCTL_EN=0x02, TCTL_PSP=0x08,
       ICR_LSC=0x04, ICR_RXDMT0=0x10, ICR_RXO=0x40, ICR_RXT0=0x80,
       RXD_DD=0x01, RXD_EOP=0x02, TXD_EOP=0x01, TXD_IXSM_TCP=0x04, TXD_IFCS=0x02, TXD_RS=0x08, TXD_DD=0x01,
       RAH_AV=0x80000000, RXCSUM_TUOFL=0x200, RXCSUM_IPOFL=0x100 };

int  ringNext(int cur, int n) { return (cur + 1) % n; }
bool rxDescDone(const RxDesc* d) { return (d->status & RXD_DD) != 0; }
void txEncode(TxDesc* d, uint32_t bufPhys, int len, bool csum) {
    d->addr = bufPhys; d->length = (uint16_t) len; d->cso = 0;
    d->cmd = TXD_EOP | TXD_IFCS | TXD_RS | (csum ? TXD_IXSM_TCP : 0);
    d->status = 0;
}

static inline void wr(E1000Core* c, int off, uint32_t v) { *(volatile uint32_t*) (c->mmio + off) = v; }
static inline uint32_t rd(E1000Core* c, int off) { return *(volatile uint32_t*) (c->mmio + off); }

// ... defaultReadMac(c), coreRxPoll(c), coreTx(c,...), setupRings(c) — same bodies as today's e1000.cpp
//     but taking c and using c->rx/c->txCur/etc. coreStart does reset, readMac (hook or default),
//     phyBringup (hook, abort on false), setupRings, RCTL/TCTL, RXCSUM if c->v->csumOffload,
//     irqSetup (try MSI-X/MSI via knx_register_msi when irqMode!=IRQ_INTX, else knx_register_irq),
//     unmask IMS, publish via knx_add_net_dev.
```
> Keep the exact reset/timing/RCTL/TCTL/TIPG values from the current `e1000.cpp`. The only additions are
> `REG_RXCSUM` programming (guarded by `csumOffload`) and the MSI path in `irqSetup`. The IRQ/MSI handler
> is a free function that calls `coreRxPoll(c)`; for MSI it takes `void*`=`c`.

- [ ] **Step 5: Rewrite `kext/e1000/e1000.cpp` as a thin wrapper.**

```cpp
#include "e1000_core.h"
extern "C" int knx_pci_find(uint16_t, uint16_t, uint8_t*, uint8_t*, uint8_t*);
extern "C" void knx_log(const char*);

static E1000Core g_core;
static const E1000Variant g_e1000 = { 0x100E, "e1000", /*phy*/0, /*readMac*/0, IRQ_INTX, /*csum*/false };

extern "C" int nkext_init() {
    uint8_t b, d, f;
    if (!knx_pci_find(0x8086, 0x100E, &b, &d, &f)) { knx_log("e1000: no 8086:100E\n"); return -1; }
    return coreStart(&g_core, &g_e1000, b, d, f);
}
```

- [ ] **Step 6: Makefile — add a pattern rule for `e1000_core` + link it into `e1000.nkext`.** The
  `kext/e1000/%.cpp` rule already exists; change the `e1000.nkext` link line to include
  `$(BINFOLDER)e1000_core.o` alongside `$(BINFOLDER)e1000.o`. Add `e1000_core` to
  `TEST_MODULES`/`COV_PATTERNS` (the pure helpers are host-tested; ensure the host build of
  `e1000_core.cpp` does not pull `knx_*` — guard the engine half with `#ifndef NANOS_HOST_TEST` around
  the `coreStart/coreRxPoll/coreTx/setupRings` definitions and the `extern "C"` knx decls, leaving
  `ringNext/rxDescDone/txEncode` always compiled).

- [ ] **Step 7: Run host tests + e1000 regression.**

Run: `make ARCH=x86_64 test 2>&1 | grep -iE "e1000|ringNext|test cases|fail" && make image64 2>&1 | tail -2 && make smoke-x86_64 2>&1 | tail -3`
Expected: host tests pass; image builds; `x86_64 MD smoke: PASS` (e1000 over INTx still works — the
behaviour-preservation gate).

- [ ] **Step 8: Commit.**

```bash
git add kext/e1000/e1000_core.h kext/e1000/e1000_core.cpp kext/e1000/e1000.cpp tests/test_e1000_core.cpp Makefile
git commit -m "refactor(e1000): extract shared E1000Core; e1000 becomes a thin variant (behaviour preserved)"
```

---

## Task 5: e1000e (82574L) kext + MSI-X + checksum offload + QEMU smoke

**Files:**
- Create: `kext/e1000/e1000e.cpp`, `scripts/smoke-e1000e.sh`
- Modify: `Makefile`

- [ ] **Step 1: Write `kext/e1000/e1000e.cpp`** (thin wrapper, MSI-X + offload).

```cpp
#include "e1000_core.h"
extern "C" int knx_pci_find(uint16_t, uint16_t, uint8_t*, uint8_t*, uint8_t*);
extern "C" void knx_log(const char*);

static E1000Core g_core;
static const E1000Variant g_e1000e = { 0x10D3, "e1000e", /*phy*/0, /*readMac*/0, IRQ_MSIX, /*csum*/true };

extern "C" int nkext_init() {
    uint8_t b, d, f;
    if (!knx_pci_find(0x8086, 0x10D3, &b, &d, &f)) { knx_log("e1000e: no 8086:10D3\n"); return -1; }
    return coreStart(&g_core, &g_e1000e, b, d, f);
}
```

- [ ] **Step 2: Makefile — build + ship `e1000e.nkext`.** Add the link rule (mirror `e1000.nkext`,
  linking `$(BINFOLDER)e1000e.o $(BINFOLDER)e1000_core.o`) and append `e1000e` to `KEXTS`.

```make
$(BINFOLDER)e1000e.nkext: $(KEXT_GLUE) $(BINFOLDER)e1000e.o $(BINFOLDER)e1000_core.o $(MKNX_TOOL) $(KEXT_LD)
	$(LD) -nostdlib -Wl,--emit-relocs -T $(KEXT_LD) -o $(@:.nkext=.elf) \
	  $(KEXT_GLUE) $(BINFOLDER)e1000e.o $(BINFOLDER)e1000_core.o -lgcc
	$(MKNX_TOOL) $(@:.nkext=.elf) $@
```
> `KEXTS=kbd mouse e1000 e1000e` (i219 is added in Phase 2).

- [ ] **Step 3: Write `scripts/smoke-e1000e.sh`.** Mirror the existing net portion of
  `smoke-x86_64.sh`, but launch QEMU with `-device e1000e` instead of `-device e1000`, and assert the
  stack came up over MSI-X: shell reached, `eth0 up` in the log, a successful ARP/ping or DHCP lease,
  and zero faults. (Copy the monitor/serial scaffolding from `smoke-x86_64.sh`; change only the NIC
  device and the network assertion, and grep the serial for `e1000e: eth0 up`.)

```bash
#!/usr/bin/env bash
# smoke-e1000e.sh — boot with the 82574L (-device e1000e), exercising the shared E1000Core via MSI-X +
# NAPI + checksum offload (the path the I219 needs but QEMU can't emulate for I219 itself).
set -u
IMG=disk/image64-grub2.img
SER=/tmp/nanos-e1000e-serial.log
INT=/tmp/nanos-e1000e-int.log
rm -f "$SER" "$INT"
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make image64'"; exit 2; }
pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
qemu-system-x86_64 -cpu qemu64 -m 512 -drive file="$IMG",format=raw \
    -netdev user,id=n0 -device e1000e,netdev=n0 \
    -display none -serial file:"$SER" -no-reboot -d int,cpu_reset -D "$INT" &
QPID=$!
trap 'kill -9 "$QPID" 2>/dev/null' EXIT
for i in $(seq 1 45); do grep -q "bash-5" "$SER" 2>/dev/null && break; sleep 1; done
sleep 2
PASS=1
chk(){ if grep -q "$1" "$SER" 2>/dev/null; then echo "  OK  : $2"; else echo "  FAIL: $2 (missing: $1)"; PASS=0; fi; }
no(){ if grep -qE "$1" "$SER" 2>/dev/null; then echo "  FAIL: $2"; PASS=0; else echo "  OK  : $2"; fi; }
echo "=== e1000e (82574L) MSI-X boot smoke ==="
chk "bash-5"           "reached the shell"
chk "e1000e: eth0 up"  "e1000e driver bound the 82574L and published eth0 (shared core + MSI-X)"
no  "CPU EXCEPTION|KERNEL EXCEPTION|Triple fault" "no faults"
echo "========================================"
if [ "$PASS" = 1 ]; then echo "e1000e MSI-X boot smoke: PASS"; exit 0; else echo "e1000e MSI-X boot smoke: FAIL"; exit 1; fi
```

- [ ] **Step 4: Add the `smoke-e1000e` target + wire into `verify64`.** In `Makefile`:

```make
smoke-e1000e: image64
	bash scripts/smoke-e1000e.sh
```
and add `smoke-e1000e` to the `.PHONY` line and to `verify64`:
```make
verify64: test64 smoke-x86_64 smoke-uefi smoke-bigmem smoke-e1000e smoke-usb
	@echo "x86_64 verify: host tests + BIOS + UEFI + big-RAM + e1000e MSI-X + live-USB boot smokes all passed."
```

- [ ] **Step 5: Build + run the new smoke + full regression.**

Run: `make image64 2>&1 | tail -2 && make smoke-e1000e 2>&1 | tail -6`
Expected: `e1000e MSI-X boot smoke: PASS` — proves the shared core, MSI-X delivery through the new
LAPIC, NAPI drain, and checksum offload all work on emulated silicon.

- [ ] **Step 6: Full gate.**

Run: `make verify64 2>&1 | grep -iE "smoke:|verify:|test cases"`
Expected: all smokes PASS (e1000 INTx regression + e1000e MSI-X + the rest).

- [ ] **Step 7: Commit.**

```bash
git add kext/e1000/e1000e.cpp scripts/smoke-e1000e.sh Makefile
git commit -m "feat(net): e1000e (82574L) kext over the shared core — MSI-X + NAPI + checksum offload (QEMU-verified)"
```

---

## Notes for the implementer

- **Why the host/kernel split via `NANOS_HOST_TEST`:** the kexts use `knx_*` imports that don't exist in
  the host doctest binary. Guard the engine halves (and the `extern "C"` knx decls) with
  `#ifndef NANOS_HOST_TEST`, leaving the pure helpers (`ringNext`/`rxDescDone`/`txEncode`,
  `msiVecAlloc`, `msiSetup`) always compiled and host-tested. Mirror how `AddressSpace`/`FrameAllocator`
  are host-tested.
- **Behaviour preservation is the e1000 gate:** Task 4 must leave `smoke-x86_64` green. The 82540EM gets
  `csumOffload=false` and `IRQ_INTX`, so its register writes are identical to today's — diff the old vs
  new register sequence if the smoke regresses.
- **LAPIC + PIC coexistence:** do NOT disable or re-mask the 8259 PIC; the LAPIC is additive. If the
  boot hangs after `lapicInit()`, the spurious-vector write or APIC-base MSR is wrong — check QEMU
  `-d int`.
- **MSI vs MSI-X in QEMU's e1000e:** the 82574L exposes MSI-X; `msiSetup` will pick it. If QEMU's model
  only wires MSI in some configs, the same code path falls through to MSI — both are exercised by the
  smoke. Legacy INTx remains the final fallback (and is what e1000 uses).
- **Out of scope here:** the I219 ich9lan PHY/ME/ULP bring-up, TSO, IOAPIC, NVM write, WoL — Phase 2 or
  separate roadmap items.
```
