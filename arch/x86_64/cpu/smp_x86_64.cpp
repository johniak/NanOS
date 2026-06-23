/*
 * smp_x86_64.cpp — x86_64 SMP bring-up: implements <arch/smp.h>.
 *
 * Drives application processors from the boot CPU (BSP) with the Intel INIT-SIPI-SIPI sequence
 * (SDM Vol.3 §8.4): copy ap_trampoline.S to its fixed low-memory home (0x8000), patch the shared
 * CR3 + C entry, then per AP patch its stack, send INIT + two SIPIs, and spin until the AP marks
 * itself online. Each AP enters apEntry64() in long mode on the BSP's kernel CR3, sets up its
 * per-CPU block + LAPIC, and either runs the registered ApEntry (Phase 3 scheduler) or idles.
 */
#include <arch/smp.h>
#include <arch/sched.h>          // archKernelCr3
#include "lapic_x86_64.h"
#include "percpu_x86_64.h"
#include "acpi_x86_64.h"         // acpiEnumCpus (MD glue)
#include <stdint.h>

namespace arch {

// Trampoline blob bounds + patch slots (ap_trampoline.S).
extern "C" unsigned char ap_trampoline_start[];
extern "C" unsigned char ap_trampoline_end[];
extern "C" uint64_t ap_pml4;        // kernel CR3
extern "C" uint64_t ap_entry64;     // &apEntry64
extern "C" uint64_t ap_stack;       // per-AP stack top

static const uint64_t AP_PHYS = 0x8000;   // SIPI vector 0x08 -> 0x8000 (page-aligned, low RAM)

static int          g_cpuCount = 1;        // BSP only until bring-up runs
static volatile int g_online   = 1;        // CPUs that have reached apEntry64 (BSP counts as 1)
static ApEntry      g_apEntry  = 0;
static uint8_t      g_lapicIds[MAX_CPUS];

// Idle/boot kernel stack per AP. The BSP runs on the loader stack (slot 0 unused). 16 KiB is
// ample for the AP idle path; Phase 3's scheduler switches each AP onto per-task kernel stacks.
static uint8_t g_apStack[MAX_CPUS][16384] __attribute__((aligned(4096)));

int  smpCpuCount() { return g_cpuCount; }
void smpSetApEntry(ApEntry fn) { g_apEntry = fn; }

int smpInit() {
    uint8_t ids[MAX_CPUS];
    int n = acpiEnumCpus(ids, MAX_CPUS, 0);   // LAPIC base already known from the NIC bring-up
    if (n <= 1) return 1;                     // uniprocessor or no ACPI/MADT
    smpBringUpAPs(ids, n);
    return __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);   // CPUs that actually came online
}

// Dense CPU index of the calling CPU. Read from the LAPIC id (always available, no MMIO side
// effects) and mapped through the enumerated id table — unlike a %gs read, this is correct in
// EVERY kernel context (syscall body, IRQ from ring 0 or 3, kernel threads), because GS is only
// the per-CPU block after a swapgs. Used by the BKL on every kernel entry, so it must never lie.
// Before bring-up (g_cpuCount == 1) the table is empty and the BSP is index 0.
int smpThisCpu() {
    uint8_t id = kernel::lapicId();
    for (int i = 0; i < g_cpuCount; i++) if (g_lapicIds[i] == id) return i;
    return 0;
}

void smpSendIpi(int cpu, uint8_t vector) {
    if (cpu < 0 || cpu >= g_cpuCount) return;
    kernel::lapicSendFixed(g_lapicIds[cpu], vector);
}

void smpTlbShootdown(uint64_t /*cr3*/) { /* Phase 4 */ }

// Entry for every AP: long mode, BSP's kernel CR3, on the stack patched into ap_stack.
extern "C" void apEntry64() {
    uint8_t id = kernel::lapicId();
    int idx = 0;
    for (int i = 0; i < g_cpuCount; i++) if (g_lapicIds[i] == id) { idx = i; break; }
    uint64_t stackTop = (uint64_t)(uintptr_t) &g_apStack[idx][sizeof(g_apStack[idx])];
    perCpuInitThis((uint32_t) idx, id, stackTop);
    kernel::lapicInit();
    __atomic_add_fetch(&g_online, 1, __ATOMIC_RELEASE);   // signal "online" to the BSP
    if (g_apEntry) g_apEntry();                            // Phase 3: enter scheduler; else idle
    for (;;) __asm__ __volatile__("hlt");
}

// Patch a trampoline slot in the COPY at AP_PHYS (the linked slot is in the high image).
static inline void patchSlot(uint64_t* linkedSlot, uint64_t value) {
    uint64_t off = (uint64_t)(uintptr_t) linkedSlot - (uint64_t)(uintptr_t) ap_trampoline_start;
    *(volatile uint64_t*)(uintptr_t)(AP_PHYS + off) = value;
}

// Crude busy-delay between INIT and SIPI (the spec wants ~10us / ~200us; bring-up is one-shot).
static void udelay(int n) { for (volatile int i = 0; i < n * 100000; i++) { } }

void smpBringUpAPs(const uint8_t* ids, int n) {
    if (n <= 1) return;
    if (n > MAX_CPUS) n = MAX_CPUS;

    // Lay the trampoline down at its fixed real-mode home and apply the shared patches.
    __builtin_memcpy((void*)(uintptr_t) AP_PHYS, ap_trampoline_start,
                     (unsigned long)(ap_trampoline_end - ap_trampoline_start));
    patchSlot(&ap_pml4, archKernelCr3());
    patchSlot(&ap_entry64, (uint64_t)(uintptr_t) &apEntry64);

    for (int i = 0; i < n; i++) g_lapicIds[i] = ids[i];
    g_cpuCount = n;

    uint8_t bsp = kernel::lapicId();
    for (int i = 0; i < n; i++) {
        if (ids[i] == bsp) continue;
        // Per-AP stack; bring-up is serialized (we wait for online below) so the shared
        // ap_stack slot is safe to reuse for the next AP.
        patchSlot(&ap_stack, (uint64_t)(uintptr_t) &g_apStack[i][sizeof(g_apStack[i])]);
        int before = __atomic_load_n(&g_online, __ATOMIC_ACQUIRE);

        kernel::lapicSendInit(ids[i]);                              udelay(1);
        kernel::lapicSendStartup(ids[i], (uint8_t)(AP_PHYS >> 12)); udelay(1);
        kernel::lapicSendStartup(ids[i], (uint8_t)(AP_PHYS >> 12)); // double SIPI (Intel-recommended)

        for (int t = 0; t < 2000000 &&
                        __atomic_load_n(&g_online, __ATOMIC_ACQUIRE) == before; t++)
            __asm__ __volatile__("pause");
    }
}

}  // namespace arch
