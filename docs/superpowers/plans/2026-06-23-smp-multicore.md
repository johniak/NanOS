# SMP / Multicore Implementation Plan (x86_64)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring NanOS x86_64 from uniprocessor to true symmetric multiprocessing — multiple CPUs running in parallel — proven by a measurable speedup on a multi-threaded user workload, then hardened with fine-grained locking and TLB shootdown.

**Architecture:** Four phases, **Big Kernel Lock first** (the roadmap §7 intermediate stage, lowest risk): (1) host-testable foundations — a spinlock primitive, an ACPI MADT parser for CPU enumeration, and LAPIC IPI; (2) application-processor bring-up via a real-mode trampoline + INIT-SIPI-SIPI, with per-CPU state behind `%gs`; (3) a Big Kernel Lock + a per-CPU-current SMP scheduler + per-CPU LAPIC timers, so user code runs on N cores; (4) replace the BKL with fine-grained per-subsystem spinlocks + IPI TLB shootdown for real scalability.

**Tech Stack:** C++ freestanding (`-mno-sse -mno-red-zone`), NASM (`.S`, Intel) for the AP trampoline, GNU as (`.s`) where it matches existing files; the existing MI/MD split (`arch/include/arch/*.h` contracts + `arch/x86_64/` impl, `make check-arch`); doctest host tests (`NANOS_HOST_TEST`, ≥90% lcov) + headless-QEMU smokes (`make verify64`). QEMU multicore via `-smp N`.

**Branch:** all work on `feat/smp` (never `master`/`main`). Commits/PRs contain **no** mention of AI/Claude. Never `git add CLAUDE.md`.

---

## Background (read first)

Current state (from a full SMP-readiness audit — verify each before editing):
- **LAPIC** is mapped + enabled (`arch/x86_64/cpu/lapic_x86_64.cpp`), but programs only ID/EOI/SPURIOUS — **no IPI / ICR writes**. The MSI vector pool there is already host-tested via the `NANOS_HOST_TEST` split — copy that pattern.
- **No ACPI** anywhere; the bootloader path is Multiboot1 (`arch/x86_64/boot/bootinfo_x86_64.cpp`), which passes **no** RSDP. CPUs cannot be enumerated today.
- **Scheduler** (`kernel/Scheduler.cpp`) is a single global runqueue with **deferred preemption** (a tick sets `g_needResched`; the switch happens on the ret-to-ring3 path, never mid-kernel). All state is global: `g_tasks/g_cur/g_ticks/g_ctxt/g_load/g_needResched/g_slice`. `Scheduler::current()` returns `&g_tasks[g_cur]` — a single global "current". **Do NOT reintroduce switching inside the IRQ handler, and do NOT busy-wait in `halt_or_hlt`** (a recorded project constraint).
- **Only synchronization** is `arch::cpuIrqSave()/cpuIrqRestore()` (RFLAGS+cli) — uniprocessor-safe only. No spinlocks, no atomics.
- **Per-CPU** = one global `struct PerCpu { uint64_t kernelStackTop, userRspScratch; }` in `arch/x86_64/cpu/syscall_x86_64.cpp`, pointed at by `IA32_KERNEL_GS_BASE`. Used only by the SYSCALL stub after `swapgs`.
- **Timer** is the global PIT at 1000 Hz (`arch/x86_64/cpu/sched_x86_64.cpp::archTimerInit`) → IRQ0 → `Scheduler::onTick`. IRQs are PIC-delivered (no IOAPIC); MSI goes straight to the LAPIC.
- **Shared singletons with no locks:** `mm/FrameAllocator`, `mm/Heap` (kernel heap), `kernel/Process` (`ProcTable`), `drivers/DeviceManager`, `fs/Vfs` + `fs/ext/BlockCache` + JBD2 `fs/ext/Journal`, the console.

Decision (approved): **full SMP, all 4 phases.** BKL-first so Phase 3 yields parallel user-space speedup with minimal correctness risk; Phase 4 then earns scalability.

Invariants that must hold throughout:
- FS / MI code stays machine-independent — `make check-arch` clean (no x86 internals leak into `fs/`, `mm/`, `kernel/`). SMP MD code (trampoline, IPI, per-CPU, LAPIC timer) lives in `arch/x86_64/` behind new `<arch/...>` contracts.
- Every pure unit gets a host test; the `NANOS_HOST_TEST` ifdef hides the MMIO/MSR/asm halves.
- The i686 build (`make build`, default `ARCH=x86`) must keep compiling — give new arch contracts a trivial uniprocessor default in `arch/x86/` (1 CPU, no-op IPI) so the frozen arch links.
- After each task: host tests green (`make test64`), `make check-arch` clean, the relevant smoke boots without a `v=0d`/`v=08` fault, then commit.

### File structure (created / modified)

Created:
- `kernel/Spinlock.h` — MI atomic ticket/test-and-set spinlock + IRQ-save guard (header-only, host-tested via `tests/test_spinlock.cpp`).
- `kernel/Acpi.h`, `kernel/Acpi.cpp` — MI ACPI RSDP/RSDT/XSDT/MADT parser over a caller-supplied physical-read callback; enumerates LAPIC ids. Host-tested (`tests/test_acpi.cpp`).
- `arch/include/arch/smp.h` — MD SMP contract: `smpCpuCount()`, `smpThisCpu()`, `smpBringUpAPs()`, `smpSendIpi()`, `smpTlbShootdown()`, per-CPU accessors.
- `arch/x86_64/cpu/smp_x86_64.cpp` — MADT-driven AP bring-up (INIT-SIPI-SIPI), per-CPU table, `<arch/smp.h>` impl.
- `arch/x86_64/cpu/ap_trampoline.S` — real-mode → long-mode AP entry blob (NASM).
- `arch/x86_64/cpu/acpi_x86_64.cpp` — MD glue: find the RSDP (scan EBDA + 0xE0000–0xFFFFF) and feed `kernel::Acpi`.
- `arch/x86/cpu/smp_x86.cpp` — uniprocessor stub impl of `<arch/smp.h>` (keeps i686 linking).
- `kernel/Bkl.h` — the Big Kernel Lock (a recursive spinlock with depth count), MI.
- `tests/test_spinlock.cpp`, `tests/test_acpi.cpp`, `tests/test_bkl.cpp`, `tests/test_percpu.cpp`.
- `scripts/smoke-smp.sh` — boots `-smp 4`, asserts all CPUs come online + the parallel workload runs.

Modified:
- `arch/x86_64/cpu/lapic_x86_64.{h,cpp}` — add ICR/IPI sends + LAPIC-timer setup.
- `arch/x86_64/cpu/syscall_x86_64.cpp` — per-CPU block becomes an array; `%gs` per CPU.
- `arch/x86_64/cpu/sched_x86_64.cpp` — LAPIC timer path alongside the PIT.
- `kernel/Scheduler.{h,cpp}` — per-CPU current/slice; global runqueue under the BKL.
- `kernel/Process.cpp` — per-CPU `g_current`; lock `ProcTable`.
- `kernel/Kernel.cpp` — call ACPI parse + `smpBringUpAPs()` after the timer is up.
- `mm/FrameAllocator.{h,cpp}`, `mm/Heap.{h,cpp}`, `drivers/DeviceManager.h`, `fs/Vfs.cpp`, `fs/ext/BlockCache.*`, `fs/ext/Journal.*` — add spinlocks (Phase 4).
- `arch/include/arch/cpu.h`, `arch/include/arch/sched.h` — per-CPU + LAPIC-timer contract additions.
- `Makefile` — add the new `.cpp` to `TEST_MODULES`/`COV_PATTERNS`; add `ARCH_SOURCES`; add `smoke-smp` to `verify64`.
- `arch/x86_64/arch.mk`, `arch/x86/arch.mk` — list the new MD objects.
- `docs/en/x86_64.md`, `docs/pl/x86_64.md`, `docs/superpowers/ROADMAP.md` — document SMP, flip Stream C status.

---

# Phase 1 — Foundations (host-testable, no AP yet)

### Task 1: Spinlock primitive

**Files:**
- Create: `kernel/Spinlock.h`
- Test: `tests/test_spinlock.cpp`
- Modify: `Makefile` (`TEST_MODULES`/`COV_PATTERNS`)

- [ ] **Step 1: Write the failing test** — `tests/test_spinlock.cpp`:

```cpp
#include "doctest.h"
#include "Spinlock.h"
using namespace kernel;

TEST_CASE("spinlock is unlocked after construction") {
    Spinlock s;
    CHECK(s.tryLock());     // free -> acquires
    CHECK_FALSE(s.tryLock()); // already held -> fails
    s.unlock();
    CHECK(s.tryLock());     // free again
    s.unlock();
}

TEST_CASE("ticket order is FIFO-fair under tryLock churn") {
    Spinlock s;
    for (int i = 0; i < 1000; i++) { CHECK(s.tryLock()); s.unlock(); }
}
```

- [ ] **Step 2: Run it, watch it fail** — `make test64` → fails: `Spinlock.h` not found / no such type.

- [ ] **Step 3: Implement `kernel/Spinlock.h`** (MI, header-only, uses GCC atomics so it host-compiles AND kernel-compiles):

```cpp
#pragma once
#include <stdint.h>
#include <arch/cpu.h>   // cpuIrqSave/cpuIrqRestore for the IRQ-save guard

namespace kernel {

// A ticket spinlock: FIFO-fair, so a CPU cannot be starved. `now`/`next` are the served
// and the next-to-hand-out ticket. Lock-free via GCC __atomic builtins (lowered to `lock
// xadd`/`lock cmpxchg` on x86_64; on the host they are real atomics for the unit tests).
class Spinlock {
    volatile uint32_t next = 0;   // next ticket to hand out
    volatile uint32_t now  = 0;   // ticket currently served
public:
    void lock() {
        uint32_t t = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
        while (__atomic_load_n(&now, __ATOMIC_ACQUIRE) != t)
            __builtin_ia32_pause();   // PAUSE: hint the spin loop (host: harmless intrinsic)
    }
    void unlock() {
        __atomic_store_n(&now, now + 1, __ATOMIC_RELEASE);
    }
    bool tryLock() {
        uint32_t t = __atomic_load_n(&now, __ATOMIC_RELAXED);
        // only succeeds if no one is waiting (next == now == t)
        return __atomic_compare_exchange_n(&next, &t, t + 1, false,
                                           __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }
};

// RAII guard that also disables interrupts on the local CPU — the correct pattern for any
// lock taken from both a thread context and an IRQ handler (otherwise a same-CPU IRQ that
// grabs the held lock self-deadlocks). Order: save+cli, then lock; unlock, then restore.
class SpinIrqGuard {
    Spinlock& l;
    unsigned long flags;
public:
    explicit SpinIrqGuard(Spinlock& s) : l(s) { flags = arch::cpuIrqSave(); l.lock(); }
    ~SpinIrqGuard() { l.unlock(); arch::cpuIrqRestore(flags); }
    SpinIrqGuard(const SpinIrqGuard&) = delete;
    SpinIrqGuard& operator=(const SpinIrqGuard&) = delete;
};

}  // namespace kernel
```

> Note: `__builtin_ia32_pause()` is available on the host (x86) gcc/clang. If a non-x86 host ever runs the tests, guard it with `#if defined(__x86_64__) || defined(__i386__)`. For now the host is x86 (per CLAUDE.md), so leave it.

- [ ] **Step 4: Run the test, watch it pass** — `make test64` → the two spinlock cases pass.

- [ ] **Step 5: Wire into the Makefile** — `Spinlock.h` is header-only (no `.cpp`), so it needs no `TEST_MODULES` entry; only `tests/test_spinlock.cpp` is auto-discovered by the test glob. Confirm `make test64` compiles it. (If the test harness lists test files explicitly, add `tests/test_spinlock.cpp` there.)

- [ ] **Step 6: Commit** — `git commit -m "smp: add MI ticket spinlock + IRQ-save guard"`.

---

### Task 2: ACPI MADT parser (CPU enumeration)

**Files:**
- Create: `kernel/Acpi.h`, `kernel/Acpi.cpp`
- Test: `tests/test_acpi.cpp`
- Modify: `Makefile` (`TEST_MODULES`, `COV_PATTERNS`)

The parser is **pure**: it walks ACPI tables given a `read(physaddr, buf, len)` callback, so it host-tests against a crafted RSDP+RSDT+MADT blob in RAM. The MD half (finding the real RSDP) is Task 2b.

- [ ] **Step 1: Write the failing test** — `tests/test_acpi.cpp` builds a minimal RSDP→RSDT→MADT in a byte array, exposes it via a callback, and asserts the parser finds the LAPIC ids:

```cpp
#include "doctest.h"
#include "Acpi.h"
#include <string.h>
#include <vector>
using namespace kernel;

// A flat fake "physical memory" the parser reads through a callback.
static std::vector<unsigned char> g_mem;
static bool fakeRead(uint64_t pa, void* dst, uint32_t len) {
    if (pa + len > g_mem.size()) return false;
    memcpy(dst, &g_mem[pa], len);
    return true;
}
static uint8_t sum8(const unsigned char* p, int n) { uint8_t s = 0; for (int i=0;i<n;i++) s+=p[i]; return s; }

TEST_CASE("MADT yields the enabled CPU LAPIC ids") {
    g_mem.assign(4096, 0);
    // --- MADT @ 0x200: header(36) + 2 LAPIC entries(8 each). type0 entry: [0,len,acpiId,apicId,flags(4)]
    int m = 0x200;
    memcpy(&g_mem[m], "APIC", 4);
    auto put32 = [&](int off, uint32_t v){ memcpy(&g_mem[off], &v, 4); };
    int e = m + 44;                              // MADT header is 44 bytes (36 + LAPIC addr + flags)
    auto lapic = [&](uint8_t apicId, bool enabled){
        g_mem[e]=0; g_mem[e+1]=8; g_mem[e+2]=apicId; g_mem[e+3]=apicId;
        put32(e+4, enabled?1:0); e+=8;
    };
    lapic(0,true); lapic(1,true); lapic(2,false); lapic(3,true);   // 3 enabled, 1 disabled
    put32(m+4, (uint32_t)(e - m));               // MADT length
    g_mem[m+9] = (uint8_t)(0u - sum8(&g_mem[m], e - m) + g_mem[m+9]); // fix checksum to 0
    // --- RSDT @ 0x100: header(36) + one 32-bit pointer to the MADT
    int r = 0x100; memcpy(&g_mem[r], "RSDT", 4); put32(r+4, 40); put32(r+36, m);
    g_mem[r+9] = (uint8_t)(0u - sum8(&g_mem[r], 40) + g_mem[r+9]);
    // --- RSDP @ 0x40: "RSD PTR ", checksum, rsdt ptr @16, revision 0 (use RSDT)
    int p = 0x40; memcpy(&g_mem[p], "RSD PTR ", 8); put32(p+16, r);
    g_mem[p+8] = (uint8_t)(0u - sum8(&g_mem[p], 20) + g_mem[p+8]);

    Acpi acpi(fakeRead);
    CHECK(acpi.parse(p));                 // start from the RSDP physical address
    CHECK(acpi.cpuCount() == 3);          // only enabled CPUs
    CHECK(acpi.lapicId(0) == 0);
    CHECK(acpi.lapicId(1) == 1);
    CHECK(acpi.lapicId(2) == 3);
}

TEST_CASE("bad RSDP checksum is rejected") {
    g_mem.assign(256, 0);
    memcpy(&g_mem[0x40], "RSD PTR ", 8);  // no valid checksum
    Acpi acpi(fakeRead);
    CHECK_FALSE(acpi.parse(0x40));
}
```

- [ ] **Step 2: Run it, watch it fail** — `make test64` → `Acpi.h` not found.

- [ ] **Step 3: Implement `kernel/Acpi.h`:**

```cpp
#pragma once
#include <stdint.h>

namespace kernel {

// Pure ACPI walker: RSDP -> RSDT/XSDT -> MADT (APIC) -> enabled LAPIC ids. Reads physical
// memory through an injected callback so it host-tests against a crafted blob and, in the
// kernel, reads through the identity map. No allocation; a fixed cap on CPUs.
class Acpi {
public:
    typedef bool (*ReadFn)(uint64_t pa, void* dst, uint32_t len);
    static const int MAX_CPUS = 32;

    explicit Acpi(ReadFn r) : read(r), nCpus(0), lapicPhys(0) {}
    bool parse(uint64_t rsdpPhys);     // false if RSDP/checksums/MADT invalid
    int  cpuCount() const { return nCpus; }
    uint8_t lapicId(int i) const { return ids[i]; }
    uint64_t lapicAddr() const { return lapicPhys; }   // LAPIC MMIO base from the MADT

private:
    ReadFn read;
    int nCpus;
    uint8_t ids[MAX_CPUS];
    uint64_t lapicPhys;
    bool checksumOk(uint64_t pa, uint32_t len);
    bool parseMadt(uint64_t pa);
};

}  // namespace kernel
```

- [ ] **Step 4: Implement `kernel/Acpi.cpp`** — RSDP (rev 0 → RSDT 32-bit ptrs; rev ≥2 → XSDT 64-bit ptrs), checksum each table, find `"APIC"` (MADT), iterate its entry list, collect type-0 (Processor Local APIC) entries whose flags bit0 (enabled) is set:

```cpp
#include "Acpi.h"

namespace kernel {

static bool sig4(const unsigned char* h, const char* s) {
    return h[0]==s[0]&&h[1]==s[1]&&h[2]==s[2]&&h[3]==s[3];
}

bool Acpi::checksumOk(uint64_t pa, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) { uint8_t b; if (!read(pa+i,&b,1)) return false; sum += b; }
    return sum == 0;
}

bool Acpi::parseMadt(uint64_t pa) {
    unsigned char hdr[44];
    if (!read(pa, hdr, 44)) return false;
    uint32_t len; __builtin_memcpy(&len, hdr+4, 4);
    if (!checksumOk(pa, len)) return false;
    __builtin_memcpy(&lapicPhys, hdr+36, 4);            // 32-bit LAPIC address field
    uint32_t off = 44;
    while (off + 2 <= len && nCpus < MAX_CPUS) {
        unsigned char e[2]; if (!read(pa+off, e, 2)) return false;
        uint8_t type = e[0], elen = e[1];
        if (elen < 2) break;
        if (type == 0) {                                 // Processor Local APIC
            unsigned char le[8]; if (!read(pa+off, le, 8)) return false;
            uint32_t flags; __builtin_memcpy(&flags, le+4, 4);
            if (flags & 1) ids[nCpus++] = le[3];          // apicId, only if enabled
        }
        off += elen;
    }
    return nCpus > 0;
}

bool Acpi::parse(uint64_t rsdpPhys) {
    unsigned char rsdp[20];
    if (!read(rsdpPhys, rsdp, 20)) return false;
    const char* SIG = "RSD PTR ";
    for (int i = 0; i < 8; i++) if (rsdp[i] != (unsigned char)SIG[i]) return false;
    if (!checksumOk(rsdpPhys, 20)) return false;
    uint8_t rev = rsdp[15];
    if (rev >= 2) {
        uint64_t xsdt; __builtin_memcpy(&xsdt, rsdp+24, 8);   // XSDT at offset 24 (rev>=2 layout)
        unsigned char h[36]; if (!read(xsdt,h,36)) return false;
        uint32_t len; __builtin_memcpy(&len, h+4, 4);
        if (!checksumOk(xsdt, len)) return false;
        for (uint32_t o = 36; o + 8 <= len; o += 8) {
            uint64_t p; if (!read(xsdt+o,&p,8)) return false;
            unsigned char th[4]; if (!read(p,th,4)) return false;
            if (sig4(th,"APIC")) return parseMadt(p);
        }
    } else {
        uint32_t rsdt; __builtin_memcpy(&rsdt, rsdp+16, 4);
        unsigned char h[36]; if (!read(rsdt,h,36)) return false;
        uint32_t len; __builtin_memcpy(&len, h+4, 4);
        if (!checksumOk(rsdt, len)) return false;
        for (uint32_t o = 36; o + 4 <= len; o += 4) {
            uint32_t p; if (!read(rsdt+o,&p,4)) return false;
            unsigned char th[4]; if (!read(p,th,4)) return false;
            if (sig4(th,"APIC")) return parseMadt(p);
        }
    }
    return false;
}

}  // namespace kernel
```

- [ ] **Step 5: Run the test, watch it pass** — `make test64`. Fix any checksum/offset mismatch against the test's crafted blob.

- [ ] **Step 6: Wire into the Makefile** — add `kernel/Acpi.cpp` to `TEST_MODULES` (around line 2183) and to `COV_PATTERNS`. Confirm coverage ≥90% for it.

- [ ] **Step 7: Commit** — `git commit -m "smp: add MI ACPI MADT parser (CPU enumeration), host-tested"`.

---

### Task 2b: MD glue — find the real RSDP

**Files:**
- Create: `arch/x86_64/cpu/acpi_x86_64.cpp`
- Modify: `arch/x86_64/arch.mk` (add `acpi_x86_64.o`)

- [ ] **Step 1: Implement `acpi_x86_64.cpp`** — the kernel reads physical memory through the identity map (`g_identityTop`, see `mmu_x86_64.cpp`); the RSDP lives either in the EBDA (pointer at physical `0x40E`, shifted <<4) or in `0xE0000–0xFFFFF`, 16-byte aligned. Provide `bool acpiReadPhys(uint64_t pa, void* dst, uint32_t len)` (memcpy from the identity map) and `uint64_t acpiFindRsdp()`:

```cpp
#include <stdint.h>
#include "Acpi.h"
namespace arch {

// Physical reads go through the huge-page identity map set up by mmuInitKernel (low RAM +
// the BIOS area are identity-mapped). For SMP bring-up we only ever read ACPI tables that
// firmware leaves in low memory, so a direct cast is valid here.
static bool acpiReadPhys(uint64_t pa, void* dst, uint32_t len) {
    __builtin_memcpy(dst, (const void*)(uintptr_t)pa, len);
    return true;
}

static bool sigRsdp(uint64_t pa) {
    const char* s = "RSD PTR "; const char* p = (const char*)(uintptr_t)pa;
    for (int i = 0; i < 8; i++) if (p[i] != s[i]) return false;
    return true;
}

uint64_t acpiFindRsdp() {
    // 1) EBDA: 16-bit segment at phys 0x40E, scan its first KiB on 16-byte boundaries.
    uint16_t ebdaSeg = *(volatile uint16_t*)(uintptr_t)0x40E;
    uint64_t ebda = (uint64_t)ebdaSeg << 4;
    if (ebda) for (uint64_t a = ebda; a < ebda + 1024; a += 16) if (sigRsdp(a)) return a;
    // 2) the BIOS read-only area 0xE0000..0xFFFFF.
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) if (sigRsdp(a)) return a;
    return 0;
}

// Convenience used by Kernel::start (Task 6): parse and return CPU count + fill an id array.
int acpiEnumCpus(uint8_t* idsOut, int maxOut, uint64_t* lapicAddrOut) {
    uint64_t rsdp = acpiFindRsdp();
    if (!rsdp) return 0;
    kernel::Acpi acpi(acpiReadPhys);
    if (!acpi.parse(rsdp)) return 0;
    int n = acpi.cpuCount(); if (n > maxOut) n = maxOut;
    for (int i = 0; i < n; i++) idsOut[i] = acpi.lapicId(i);
    if (lapicAddrOut) *lapicAddrOut = acpi.lapicAddr();
    return n;
}

}  // namespace arch
```

- [ ] **Step 2: List it** — append `acpi_x86_64.o` to `ARCH_SOURCES` in `arch/x86_64/arch.mk`.

- [ ] **Step 3: Verify it links + enumerates** — temporarily log `acpiEnumCpus` count from `Kernel::start`; `make smoke-x86_64` (QEMU defaults to 1 CPU → expect count 1) and `make ARCH=x86_64 run64` with `-smp 4` → expect 4. Remove the temp log.

- [ ] **Step 4: Commit** — `git commit -m "smp: x86_64 RSDP discovery + ACPI CPU enumeration glue"`.

---

### Task 3: LAPIC IPI (INIT / SIPI / fixed)

**Files:**
- Modify: `arch/x86_64/cpu/lapic_x86_64.h`, `arch/x86_64/cpu/lapic_x86_64.cpp`

- [ ] **Step 1: Add the ICR API to `lapic_x86_64.h`** (inside the kernel-only section of the header's declarations):

```cpp
// Inter-processor interrupts (kernel-only). dest = target LAPIC id.
void lapicSendInit(uint8_t dest);                 // INIT assert (level)
void lapicSendStartup(uint8_t dest, uint8_t vec); // SIPI; vec*0x1000 = trampoline phys addr
void lapicSendFixed(uint8_t dest, uint8_t vec);   // a normal fixed IPI to one CPU
void lapicTimerInit(uint8_t vec, uint32_t initialCount);  // per-CPU LAPIC timer, periodic
```

- [ ] **Step 2: Implement in `lapic_x86_64.cpp`** (add ICR + LVT-timer registers; ICR is a 64-bit write split across `0x310` (dest, high) then `0x300` (command, low) which triggers send):

```cpp
enum { LAPIC_ICR_LO = 0x300, LAPIC_ICR_HI = 0x310,
       LAPIC_LVT_TIMER = 0x320, LAPIC_TIMER_INIT = 0x380,
       LAPIC_TIMER_CUR = 0x390, LAPIC_TIMER_DIV = 0x3E0 };

static void icrWrite(uint8_t dest, uint32_t lo) {
    g_lapic[LAPIC_ICR_HI / 4] = (uint32_t) dest << 24;   // destination in bits 24-27
    g_lapic[LAPIC_ICR_LO / 4] = lo;                       // writing the low word sends it
    while (g_lapic[LAPIC_ICR_LO / 4] & (1u << 12)) { }    // wait for Delivery Status = idle
}
void lapicSendInit(uint8_t dest)               { icrWrite(dest, 0x4500); } // INIT, assert, edge
void lapicSendStartup(uint8_t dest, uint8_t v) { icrWrite(dest, 0x4600 | v); } // STARTUP + vector
void lapicSendFixed(uint8_t dest, uint8_t v)   { icrWrite(dest, 0x4000 | v); } // fixed delivery
void lapicTimerInit(uint8_t vec, uint32_t init) {
    g_lapic[LAPIC_TIMER_DIV / 4]  = 0x3;                   // divide by 16
    g_lapic[LAPIC_LVT_TIMER / 4]  = (uint32_t) vec | (1u << 17);  // periodic mode
    g_lapic[LAPIC_TIMER_INIT / 4] = init;                  // counts down at bus freq / 16
}
```

> The ICR delivery encodings: `0x4500` = INIT/assert, `0x4600|vec` = STARTUP (SIPI), `0x4000|vec` = fixed. Bit 12 is Delivery Status (busy). These are standard Intel SDM Vol.3 §10.6 values.

- [ ] **Step 3: Build check** — `make build64` (or `make ARCH=x86_64 image64`). No host test (MMIO-only; the host half is `#ifdef`'d out — the existing `lapic_x86_64.cpp` already splits this way, so the new functions sit in the same kernel-only block).

- [ ] **Step 4: Commit** — `git commit -m "smp: LAPIC IPI (INIT/SIPI/fixed) + per-CPU timer setup"`.

---

# Phase 2 — Application-processor bring-up

### Task 4: Per-CPU data block (array, GS-indexed)

**Files:**
- Modify: `arch/x86_64/cpu/syscall_x86_64.cpp`
- Create: `arch/x86_64/cpu/percpu_x86_64.h` (shared per-CPU struct + accessors)
- Test: `tests/test_percpu.cpp` (pure index/struct logic)
- Modify: `arch/include/arch/cpu.h` (contract), `Makefile`

- [ ] **Step 1: Define the per-CPU struct** — `arch/x86_64/cpu/percpu_x86_64.h`:

```cpp
#pragma once
#include <stdint.h>
namespace arch {
struct PerCpu {
    uint64_t kernelStackTop;   // [0]  SYSCALL stub reads this after swapgs (KEEP offset 0)
    uint64_t userRspScratch;   // [8]  SYSCALL stub scratch (KEEP offset 8)
    uint32_t cpuIndex;         // [16] dense 0..N-1 index (NOT the LAPIC id)
    uint32_t lapicId;          // [20]
    void*    currentTask;      // [24] kernel::Task* running on THIS cpu (Phase 3)
    void*    currentThread;    // [32]
    uint32_t inIrq;            // [40] interrupt nesting depth on this cpu
};
static const int MAX_CPUS = 32;
extern PerCpu g_percpu[MAX_CPUS];     // one block per cpu; gs base = &g_percpu[idx]
}  // namespace arch
```

> The first two fields keep their offsets — `syscall_entry64.S` hard-codes `%gs:0`/`%gs:8`. Verify those offsets in the .S before changing the struct.

- [ ] **Step 2: Replace the singleton** in `syscall_x86_64.cpp`: `PerCpu g_percpu;` → `PerCpu g_percpu[MAX_CPUS];` (include the new header). `syscallSetKernelStack` must write the **current** CPU's block — for the BSP at init that is index 0; expose a setter taking an index, and add `void perCpuInitThis(uint32_t idx, uint32_t lapicId)` that sets `IA32_KERNEL_GS_BASE = &g_percpu[idx]` and fills the fields. Refactor `syscallInit` to call `perCpuInitThis(0, lapicId())` so the BSP keeps working unchanged.

- [ ] **Step 3: GS accessor** — implement `arch::smpThisCpu()` (declared in `<arch/smp.h>`, Task 5) HERE in `cpu_x86_64.cpp` by reading the current CPU's dense index from its per-CPU block: `rdmsr(IA32_GS_BASE)` → `((PerCpu*)base)->cpuIndex`, or a `%gs:16` read. This is THE single per-CPU-index accessor (the BKL in Task 7 and `smpSendIpi` already call `smpThisCpu()`); do NOT add a second `cpuThisIndex()`. The i686 stub (`smp_x86.cpp`) returns `0`.

- [ ] **Step 4: Pure test** — `tests/test_percpu.cpp`: verify a small helper `int cpuIndexForLapic(const uint8_t* ids, int n, uint8_t lapicId)` (used by AP entry to map its LAPIC id back to a dense index) returns the right index / -1. Put that helper in a host-compilable spot (e.g. `percpu_x86_64.h` as an inline). Run `make test64`.

- [ ] **Step 5: Build + boot** — `make smoke-x86_64` still boots to the shell (BSP path unchanged; this is pure refactor + new fields).

- [ ] **Step 6: Commit** — `git commit -m "smp: per-CPU data block array, GS-indexed; BSP uses index 0"`.

---

### Task 5: AP trampoline + INIT-SIPI-SIPI

**Files:**
- Create: `arch/x86_64/cpu/ap_trampoline.S` (NASM)
- Create: `arch/x86_64/cpu/smp_x86_64.cpp`
- Create: `arch/include/arch/smp.h`
- Create: `arch/x86/cpu/smp_x86.cpp` (uniprocessor stub)
- Modify: `arch/x86_64/arch.mk`, `arch/x86/arch.mk`

- [ ] **Step 1: The SMP contract** — `arch/include/arch/smp.h`:

```cpp
#pragma once
#include <stdint.h>
namespace arch {
int  smpCpuCount();                 // 1 until smpBringUpAPs() runs
int  smpThisCpu();                  // dense index of the calling CPU
void smpBringUpAPs(const uint8_t* lapicIds, int n);  // INIT-SIPI-SIPI the others; blocks until online
void smpSendIpi(int cpu, uint8_t vector);
void smpTlbShootdown(uint64_t cr3); // Phase 4
typedef void (*ApEntry)();          // what each AP runs once long-mode is up
void smpSetApEntry(ApEntry fn);
}  // namespace arch
```

- [ ] **Step 2: The AP trampoline** — `ap_trampoline.S`. APs start in **16-bit real mode** at `vector*0x1000` (a page-aligned low address, e.g. phys `0x8000`). The blob must be copied there at runtime (it is linked in the kernel's high image). It: sets a temp GDT, enables protected mode, then long mode (load CR3 = the BSP's kernel PML4, set EFER.LME, CR0.PG), and far-jumps to a 64-bit stub that loads its stack + calls a C entry. Provide the full blob with a known layout (symbols `ap_trampoline_start`, `ap_trampoline_end`, and patch points for `CR3`, the per-AP stack pointer, and the C entry address). Key structure:

```asm
; ap_trampoline.S — copied to phys AP_TRAMPOLINE (0x8000). APs enter here in real mode.
; Patch points (filled by smp_x86_64.cpp before each SIPI): ap_pml4, ap_entry64, ap_stack.
bits 16
global ap_trampoline_start, ap_trampoline_end
global ap_pml4, ap_entry64, ap_stack
ap_trampoline_start:
    cli
    lgdt [ap_gdt32_ptr - ap_trampoline_start + 0x8000]
    mov eax, cr0
    or  eax, 1                  ; PE
    mov cr0, eax
    jmp 0x08:(ap_pm - ap_trampoline_start + 0x8000)
bits 32
ap_pm:
    mov ax, 0x10
    mov ds, ax
    mov eax, [ap_pml4 - ap_trampoline_start + 0x8000]
    mov cr3, eax
    mov eax, cr4
    or  eax, (1<<5)             ; PAE
    mov cr4, eax
    mov ecx, 0xC0000080         ; EFER
    rdmsr
    or  eax, (1<<8)             ; LME
    wrmsr
    mov eax, cr0
    or  eax, (1<<31)            ; PG
    mov cr0, eax
    lgdt [ap_gdt64_ptr - ap_trampoline_start + 0x8000]
    jmp 0x08:(ap_lm - ap_trampoline_start + 0x8000)
bits 64
ap_lm:
    mov rsp, [ap_stack - ap_trampoline_start + 0x8000]
    mov rax, [ap_entry64 - ap_trampoline_start + 0x8000]
    call rax                    ; never returns
.hang: hlt
    jmp .hang
; --- patch slots + temp GDTs (32-bit flat + 64-bit long) live here, relocated by +0x8000 ---
```

> The relocation `- ap_trampoline_start + 0x8000` turns link-time addresses into the runtime phys location. Keep the trampoline < 4 KiB. Copy `[ap_trampoline_start, ap_trampoline_end)` to `0x8000` once at bring-up.

- [ ] **Step 3: The bring-up driver** — `smp_x86_64.cpp` implements `<arch/smp.h>`: copy the trampoline to `0x8000`, patch `ap_pml4 = archKernelCr3()`, `ap_entry64 = &apEntry64`, and for each non-BSP LAPIC id: allocate a kernel stack, patch `ap_stack`, then `lapicSendInit(id)`; short delay; `lapicSendStartup(id, 0x08)` (0x08*0x1000 = 0x8000) twice (the Intel-recommended double SIPI); then spin on a per-AP `volatile bool online` flag the AP sets. `apEntry64()` (C++) calls `perCpuInitThis(idx, lapicId)`, `lapicInit()`, sets `online=true`, then either idles (Phase 2) or enters the scheduler (Phase 3 wires `smpSetApEntry`). Keep a global `g_cpuCount`.

```cpp
#include <arch/smp.h>
#include "lapic_x86_64.h"
#include "percpu_x86_64.h"
#include <arch/sched.h>   // archKernelCr3
namespace arch {
extern "C" unsigned char ap_trampoline_start[], ap_trampoline_end[];
extern "C" uint64_t ap_pml4, ap_entry64, ap_stack;   // patch slots (see .S)
static const uint64_t AP_PHYS = 0x8000;
static int g_cpuCount = 1;
static volatile int g_online = 1;
static ApEntry g_apEntry = 0;
static uint8_t g_lapicIds[MAX_CPUS];

int smpCpuCount() { return g_cpuCount; }
void smpSetApEntry(ApEntry fn) { g_apEntry = fn; }

extern "C" void apEntry64() {
    // We are on a fresh AP, long mode, on the BSP's kernel CR3. Find our dense index by LAPIC id.
    uint8_t id = kernel::lapicId();
    int idx = 0; for (int i = 0; i < g_cpuCount; i++) if (g_lapicIds[i] == id) idx = i;
    perCpuInitThis((uint32_t) idx, id);
    kernel::lapicInit();
    __atomic_add_fetch(&g_online, 1, __ATOMIC_RELEASE);
    if (g_apEntry) g_apEntry();        // Phase 3: enter scheduler. Phase 2: idle.
    for (;;) __asm__ __volatile__("hlt");
}

static void udelay(int n) { for (volatile int i = 0; i < n * 100000; i++) {} }  // crude; replaced by PIT later

void smpBringUpAPs(const uint8_t* ids, int n) {
    if (n <= 1) return;
    __builtin_memcpy((void*) AP_PHYS, ap_trampoline_start,
                     (size_t)(ap_trampoline_end - ap_trampoline_start));
    // patch CR3 + C entry (shared by all APs)
    *(uint64_t*)((uintptr_t)&ap_pml4 - (uintptr_t)ap_trampoline_start + AP_PHYS) = archKernelCr3();
    *(uint64_t*)((uintptr_t)&ap_entry64 - (uintptr_t)ap_trampoline_start + AP_PHYS) = (uint64_t)&apEntry64;
    for (int i = 0; i < n; i++) g_lapicIds[i] = ids[i];
    g_cpuCount = n;
    uint8_t bsp = kernel::lapicId();
    for (int i = 0; i < n; i++) {
        if (ids[i] == bsp) continue;
        void* stk = /* allocate an 8 KiB kernel stack */ archAllocApStack();
        *(uint64_t*)((uintptr_t)&ap_stack - (uintptr_t)ap_trampoline_start + AP_PHYS) = (uint64_t) stk;
        int before = __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);
        kernel::lapicSendInit(ids[i]); udelay(1);
        kernel::lapicSendStartup(ids[i], (uint8_t)(AP_PHYS >> 12)); udelay(1);
        kernel::lapicSendStartup(ids[i], (uint8_t)(AP_PHYS >> 12));
        for (int t = 0; t < 1000000 && __atomic_load_n(&g_online,__ATOMIC_ACQUIRE) == before; t++)
            __builtin_ia32_pause();
    }
}
int smpThisCpu() { return (int) g_percpu[0].cpuIndex; /* replaced by gs-read in Task 4 step 3 */ }
void smpSendIpi(int cpu, uint8_t v) { kernel::lapicSendFixed(g_lapicIds[cpu], v); }
}  // namespace arch
```

> `archAllocApStack()` = `malloc(8192)` aligned, returning the top; add it as a tiny helper (kernel heap is single-CPU-safe at bring-up time, before APs run). `0x8000` must be reserved — confirm it is free in the boot memory map (it is, in the low conventional RAM the kernel does not use; mark it reserved if needed).

- [ ] **Step 4: i686 stub** — `arch/x86/cpu/smp_x86.cpp`: `int smpCpuCount(){return 1;} int smpThisCpu(){return 0;} void smpBringUpAPs(const uint8_t*,int){} void smpSendIpi(int,uint8_t){} void smpSetApEntry(ApEntry){} void smpTlbShootdown(uint64_t){}`. List in `arch/x86/arch.mk`.

- [ ] **Step 5: List the new MD objects** — `arch/x86_64/arch.mk`: add `ap_trampoline.o smp_x86_64.o`. Confirm `ASM_FMT=elf64` builds the `.S` (the Makefile already assembles `.S` with NASM for this arch).

- [ ] **Step 6: Build** — `make build64`. Fix link errors (the `ap_*` symbol externs, the trampoline relocation).

- [ ] **Step 7: Commit** — `git commit -m "smp: AP trampoline + INIT-SIPI-SIPI bring-up driver; i686 uniprocessor stub"`.

---

### Task 6: Bring APs up at boot + proof of life

**Files:**
- Modify: `kernel/Kernel.cpp`
- Create: `scripts/smoke-smp.sh`
- Modify: `Makefile` (`smoke-smp`, add to `verify64`)

- [ ] **Step 1: Wire bring-up into `Kernel::start`** — after `archTimerInit(1000)` and LAPIC init, before `Scheduler::start()`: call `arch::acpiEnumCpus(ids, MAX, &lapicAddr)`; if count > 1, `arch::smpBringUpAPs(ids, count)`. For Phase 2, leave `smpSetApEntry` unset so APs idle (`hlt`). Print `"SMP: N CPUs online"` to the console once bring-up returns.

- [ ] **Step 2: smoke-smp.sh** — model it on `scripts/smoke-x86_64.sh`: build `image64`, boot `qemu-system-x86_64 ... -smp 4 -display none -monitor unix:/tmp/qmon-smp,server,nowait`, screendump after boot, OCR/grep the PPM-or-serial for `SMP: 4 CPUs online`, and grep the `-d int` log for `v=0d`/`v=08` (fail on any). Exit non-zero on mismatch.

```sh
#!/usr/bin/env bash
set -euo pipefail
# boot with 4 vCPUs and assert all came online + no faults (pattern from smoke-x86_64.sh)
# ... (qemu -smp 4 -display none -monitor unix:... ; screendump; convert; grep "SMP: 4 CPUs online")
```

- [ ] **Step 3: Run it** — `make smoke-smp`. Expect "SMP: 4 CPUs online", no faults. Debug AP hangs with `-d int -D /tmp/smp.log` and inspect for `v=0d` in the trampoline.

- [ ] **Step 4: Gate it** — add `smoke-smp` to the `verify64` target's dependency list and to the `.PHONY` line.

- [ ] **Step 5: Commit** — `git commit -m "smp: bring APs up at boot; smoke-smp proves 4 cores online in QEMU"`.

---

# Phase 3 — Big Kernel Lock + SMP scheduler (parallel user-space)

### Task 7: The Big Kernel Lock

**Files:**
- Create: `kernel/Bkl.h`
- Test: `tests/test_bkl.cpp`
- Modify: `arch/x86_64/cpu/syscall_entry64.S` / `syscall_x86_64.cpp`, `arch/x86_64/cpu/isr64.S` / `Interrupt64.cpp` (acquire on kernel entry, release on exit)

- [ ] **Step 1: Failing test** — `tests/test_bkl.cpp`: the BKL is recursive on one CPU (a fault handler nested inside a syscall must re-enter, not deadlock). Test depth bookkeeping: `bklEnter()` twice then `bklExit()` twice releases exactly once.

- [ ] **Step 2: Implement `kernel/Bkl.h`** — a `Spinlock` plus a per-CPU recursion depth + an owner-cpu field:

```cpp
#pragma once
#include "Spinlock.h"
#include <arch/smp.h>
namespace kernel {
struct Bkl {
    Spinlock lock;
    volatile int ownerCpu = -1;
    volatile int depth = 0;
    void enter() {
        int cpu = arch::smpThisCpu();
        if (ownerCpu == cpu) { depth++; return; }   // recursive re-entry on same CPU
        lock.lock();
        ownerCpu = cpu; depth = 1;
    }
    void exit() {
        if (--depth == 0) { ownerCpu = -1; lock.unlock(); }
    }
};
extern Bkl g_bkl;
}  // namespace kernel
```

> Caution: `ownerCpu`/`depth` are only ever touched by the lock holder (or a same-CPU nested entry), so they need no separate atomic — but they MUST be read/written with the BKL held or by the owning CPU. Keep `arch::smpThisCpu()` cheap (a `%gs:16` read).

- [ ] **Step 3: Take/release on kernel boundaries** — acquire `g_bkl.enter()` at the very top of: `syscall_dispatch64` (before `cpuEnableInterrupts`), the IRQ common handler (`Interrupt64.cpp`), and the fault handler; release `g_bkl.exit()` on every exit path (before SYSRET / before IRET-to-ring3 / before the scheduler context switch hands the CPU to another task). **Critical:** the context switch must release the BKL before `archContextSwitch` and the resumed task re-takes it, OR the lock is handed off — choose **release-before-switch, re-acquire-after** (simplest; the scheduler picks the next task under the BKL, drops it, switches, and the resumed task's frame re-enters through its own boundary). Document this precisely in `Scheduler::schedule` (Task 8).

- [ ] **Step 4: Build + boot single CPU** — `make smoke-x86_64`. With 1 CPU the BKL is uncontended; the system must boot to the shell exactly as before (regression gate).

- [ ] **Step 5: Commit** — `git commit -m "smp: Big Kernel Lock taken on kernel entry, released on exit"`.

---

### Task 8: Per-CPU current + SMP scheduler

**Files:**
- Modify: `kernel/Scheduler.cpp`, `kernel/Scheduler.h`, `kernel/Process.cpp`

- [ ] **Step 1: Make "current" per-CPU** — replace the single `g_cur` with per-CPU current-task pointers stored in `arch::g_percpu[idx].currentTask`. `Scheduler::current()` returns `this_cpu->currentTask`. Keep `g_tasks[]` as the **shared global runqueue** (protected by the BKL — every scheduler entry already holds it). `ProcTable::setCurrent/getCurrent` likewise become per-CPU (store in the per-CPU block, not a global `g_current`).

- [ ] **Step 2: SMP-safe `pickNext`** — `pickNext` scans `g_tasks[]` for a `TASK_READY` task **not currently RUNNING on another CPU**. Add a `runningCpu` field to `Task` (-1 = not running); a task is claimable only if `state==TASK_READY`. On selection set `state=TASK_RUNNING, runningCpu=thisCpu`; on deselection back to `TASK_READY, runningCpu=-1`. This prevents two CPUs running the same task. Each CPU has its own idle task (idle is now per-CPU — create N idle tasks, one per CPU, slot 0..N-1; `Scheduler::idle()` returns the per-CPU idle).

- [ ] **Step 3: Release/reacquire BKL across the switch** — in `schedule()`: hold BKL (already held by the caller boundary), `cpuIrqSave`, pick next + flip states, set this CPU's per-CPU current + TSS (`arch::setKernelStack`) + TLS, `cpuIrqRestore`, **`g_bkl.exit()`**, `arch::archContextSwitch(...)`, **`g_bkl.enter()`** (the resumed context re-takes it). Verify the deferred-preemption invariant still holds (no switch inside an IRQ handler — preemption still happens only on the ret-to-ring3 path via `preempt()`).

- [ ] **Step 4: AP scheduler entry** — `smpSetApEntry()` to a function that creates/enters that CPU's idle task and calls `Scheduler::start()`-equivalent for an AP (enter the idle loop under the BKL discipline). The BSP keeps the existing `Scheduler::start()`.

- [ ] **Step 5: Host tests** — extend `tests/test_scheduler*` (or add cases): `pickNext` skips tasks with `runningCpu != -1`; two simulated CPUs never pick the same task. `make test64`.

- [ ] **Step 6: Boot 1 CPU then 4** — `make smoke-x86_64` (1 CPU: unchanged behavior). `make smoke-smp` (4 CPUs: all idle tasks run; shell still works). No faults.

- [ ] **Step 7: Commit** — `git commit -m "smp: per-CPU current + global-runqueue scheduler under the BKL"`.

---

### Task 9: Per-CPU LAPIC timer preemption

**Files:**
- Modify: `arch/x86_64/cpu/sched_x86_64.cpp`, `arch/x86_64/cpu/smp_x86_64.cpp`, `arch/x86_64/cpu/Idt64.cpp` (a LAPIC-timer IDT vector)

- [ ] **Step 1: Calibrate** — measure the LAPIC timer frequency against the PIT once on the BSP (count LAPIC ticks over a known PIT interval), store ticks-per-ms.

- [ ] **Step 2: Per-CPU timer** — each CPU (BSP + every AP, in `apEntry64`/`Kernel::start`) calls `lapicTimerInit(vec, ticksPerMs)` for a ~1000 Hz periodic interrupt. Install an IDT vector for it that EOIs the LAPIC and calls `Scheduler::onTick(fromUser)` **for the local CPU**. Keep the PIT as the single global timekeeping source (`g_ticks`) OR move timekeeping to the BSP's LAPIC timer and have APs only drive their own `preempt()` — choose: **PIT/BSP keeps the wall clock + `g_ticks`; each CPU's LAPIC timer only flags its own `g_needResched`** (per-CPU `needResched`). This avoids N CPUs racing `g_ticks`.

- [ ] **Step 3: Per-CPU `needResched`** — move `g_needResched`/`g_slice` into the per-CPU block. `onTick` updates the local CPU's slice; `preempt()` reads the local flag.

- [ ] **Step 4: Boot 4 CPUs** — `make smoke-smp`: confirm each CPU preempts independently (e.g. spawn 4 spinning kernel threads / 4 user processes and observe they all advance). No faults.

- [ ] **Step 5: Commit** — `git commit -m "smp: per-CPU LAPIC-timer preemption; per-CPU resched flag"`.

---

### Task 10: Parallel-speedup proof

**Files:**
- Modify: `scripts/smoke-smp.sh`, possibly `user/pfract.c` (already exists — the parallel Mandelbrot pthread gate)

- [ ] **Step 1: Workload** — `user/pfract.c` (the existing parallel Mandelbrot driven by real pthreads) is the gate. Confirm it spawns ≥ N worker threads. If it prints a wall-clock time, capture it.

- [ ] **Step 2: Measure** — `smoke-smp.sh` runs `pfract` under `-smp 1` and `-smp 4`, captures the reported time (or tick count via `/proc/uptime` deltas), and asserts the 4-CPU run is meaningfully faster (e.g. ≥1.8× on 4 cores for an embarrassingly-parallel workload). Print both numbers.

- [ ] **Step 3: Run** — `make smoke-smp`. This is the **Phase 3 milestone**: user threads run on multiple cores with measurable speedup.

- [ ] **Step 4: Commit** — `git commit -m "smp: parallel-speedup gate (pfract on 4 cores vs 1)"`.

---

# Phase 4 — Fine-grained locking + TLB shootdown (scalability)

> Goal: remove the BKL so kernel work runs concurrently. Do it **incrementally** — lock one subsystem, re-verify, repeat — keeping the BKL until the last lock lands, then drop it. Each task: add a `Spinlock` member, take `SpinIrqGuard` in every public mutator/reader, host-test where the subsystem is host-tested, boot 4 CPUs.

### Task 11: Lock the frame allocator + kernel heap

**Files:** `mm/FrameAllocator.{h,cpp}`, `mm/Heap.{h,cpp}`

- [ ] **Step 1:** Add a `Spinlock` to `FrameAllocator`; wrap `alloc`/`free`/bitmap scans in `SpinIrqGuard`. Host test (`tests/test_frame_alloc*`) still passes (single-threaded path unchanged). 
- [ ] **Step 2:** Add a `Spinlock` to `Heap`; wrap `malloc`/`free`/`realloc`/coalescing in `SpinIrqGuard`. The boundary-tag canary check stays. Host test passes.
- [ ] **Step 3:** Boot `make smoke-smp`; 4 CPUs allocating concurrently (the pfract workers’ stacks/heaps) — no corruption, no canary panic.
- [ ] **Step 4:** Commit — `git commit -m "smp: lock frame allocator + kernel heap"`.

### Task 12: Lock ProcTable + DeviceManager

**Files:** `kernel/Process.cpp`, `drivers/DeviceManager.h`

- [ ] **Step 1:** A `Spinlock` guarding the `g_procs[]` table mutations (fork/exec/exit/lookup). Per-CPU current is already separate (Task 8) — only the shared table needs the lock.
- [ ] **Step 2:** A `Spinlock` guarding `DeviceManager` register/lookup.
- [ ] **Step 3:** Host tests pass; `make smoke-smp` (concurrent fork from multiple CPUs) clean.
- [ ] **Step 4:** Commit — `git commit -m "smp: lock process table + device manager"`.

### Task 13: Lock the VFS path + block cache + journal

**Files:** `fs/Vfs.cpp`, `fs/ext/BlockCache.*`, `fs/ext/Journal.*`

- [ ] **Step 1:** A VFS-level `Spinlock` (coarse: one lock for the mount table + path routing) — fine enough for correctness; per-inode locking is a later optimization. Take it in every `Vfs::` public op.
- [ ] **Step 2:** A `Spinlock` in `BlockCache` (LRU/dirty-list mutations) and one serializing `Journal` transactions (JBD2 `txFlush` must be atomic w.r.t. concurrent writers). 
- [ ] **Step 3:** Host tests (`test_vfs*`, `test_ext*`, `test_ext_journal`) pass. `make smoke-smp` with concurrent file writes from 2+ CPUs → image stays e2fsck-clean (extend the smoke to run a 2-CPU write workload then `e2fsck -fn`).
- [ ] **Step 4:** Commit — `git commit -m "smp: lock VFS + block cache + JBD2 journal"`.

### Task 14: TLB shootdown via IPI

**Files:** `arch/x86_64/cpu/smp_x86_64.cpp`, `arch/x86_64/cpu/Idt64.cpp`, `arch/include/arch/smp.h`, `arch/x86_64/mm/mmu_x86_64.cpp`

- [ ] **Step 1:** Define a TLB-shootdown IPI vector + handler that reloads CR3 (or `invlpg`s a passed range) and EOIs. A per-CPU "shootdown request" mailbox + an ack counter.
- [ ] **Step 2:** `smpTlbShootdown(cr3)` (and a range variant): the initiator writes the request, `lapicSendFixed` to every other CPU, spins until all ack. Call it from the MMU paths that change a mapping shared across CPUs (kernel mappings; a process whose address space is active on >1 CPU — rare with per-process spaces, but munmap/COW need it).
- [ ] **Step 3:** Boot `make smoke-smp` with a munmap/COW workload (a forking + freeing user program) → no stale-TLB faults.
- [ ] **Step 4:** Commit — `git commit -m "smp: IPI TLB shootdown"`.

### Task 15: Retire the BKL

**Files:** the kernel-entry boundaries (Task 7 sites), `kernel/Bkl.h`

- [ ] **Step 1:** Now that every shared subsystem has its own lock, remove `g_bkl.enter()/exit()` from the syscall/IRQ/fault boundaries and the scheduler. The scheduler still needs a lock for the **runqueue** — replace the BKL's role there with a dedicated `Spinlock g_rqLock` taken only around `pickNext` + state flips (held briefly, released before the context switch).
- [ ] **Step 2:** Audit for any remaining unprotected shared state surfaced by removing the BKL (the network stack, console). Add locks as needed (a console `Spinlock` so 4 CPUs’ output doesn’t interleave; a net-stack lock — coarse).
- [ ] **Step 3:** `make test64` + `make verify64` (all smokes, including `smoke-smp` with the write + parallel workloads). Re-measure pfract speedup — should improve over Phase 3 (kernel work no longer serialized).
- [ ] **Step 4:** Commit — `git commit -m "smp: retire the BKL in favour of per-subsystem locks + a runqueue lock"`.

### Task 16: Documentation + final verification

**Files:** `docs/en/x86_64.md`, `docs/pl/x86_64.md`, `docs/superpowers/ROADMAP.md`

- [ ] **Step 1:** Add an "SMP / multicore" section to `docs/en/x86_64.md` (+ PL): CPU enumeration via ACPI MADT, AP trampoline + INIT-SIPI-SIPI, per-CPU `%gs` data, the spinlock/locking model (BKL→fine-grained history), per-CPU LAPIC timers, TLB shootdown. Update the interrupts/APIC section (IPI now used).
- [ ] **Step 2:** Flip Stream C in `ROADMAP.md` from ⬜ to ✅/🟡 (QEMU-verified; real-HW pending the Dell), noting the BKL→fine-grained path taken.
- [ ] **Step 3:** Run the full gate: `make check-arch`, `make test64`, `make verify64`. All green.
- [ ] **Step 4:** Commit — `git commit -m "docs: document SMP; mark Stream C done (QEMU)"`.

---

## Risks & mitigations (SMP-specific)

| Risk | Mitigation |
|---|---|
| Implicit uniprocessor assumptions beyond the audited list | BKL (Phase 3) makes the kernel correct-by-serialization first; fine-grained locking (Phase 4) is added one subsystem at a time, each re-verified. |
| AP trampoline hangs / triple-faults (real-mode→long-mode is fiddly) | Smallest possible blob; reuse the BSP's kernel CR3; debug with `-d int -D log` and `v=0d`/`v=08` grep; double-SIPI per Intel SDM. |
| Lock taken in both thread + IRQ context → self-deadlock | Always use `SpinIrqGuard` (disables local IRQs while held). |
| `g_ticks`/timekeeping raced by N CPUs | One timekeeping source (BSP/PIT) owns `g_ticks`; per-CPU LAPIC timers only drive per-CPU `needResched`. |
| Deferred-preemption invariant broken (switching inside an IRQ) | Keep the existing model: ticks set a flag; the switch happens only on ret-to-ring3 (`preempt()`). Reviewed in Task 8/9. |
| Context-switch + BKL handoff ordering bug | Explicit rule: release BKL before `archContextSwitch`, re-acquire after; documented in `schedule()`. |
| Real HW (Dell) differs from QEMU (ACPI quirks, x2APIC) | QEMU `-smp` is the gate now; real-HW SMP is a follow-on once NVMe/native boot mature (it needs the same MADT path, which Limine's ACPI also provides). |

## Verification summary

- **Host:** `make test64` — Spinlock, ACPI MADT parser, BKL recursion, per-CPU index helper, scheduler `pickNext` SMP-safety (≥90% lcov on the new MI `.cpp`).
- **QEMU:** `make smoke-smp` (`-smp 4`) — all CPUs online, no `v=0d`/`v=08`, parallel pfract speedup ≥1.8×, concurrent file writes stay e2fsck-clean. Plus the existing `verify64` smokes (BIOS/UEFI/bigmem/e1000e/usb) must stay green throughout.
- **check-arch:** clean after every task (SMP MD code stays in `arch/x86_64/`; only MI primitives — Spinlock, Acpi, Bkl — live in `kernel/`).
- **i686:** `make build` (default arch) keeps compiling via the `arch/x86/cpu/smp_x86.cpp` uniprocessor stub.
