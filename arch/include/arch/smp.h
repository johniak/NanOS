/*
 * arch/smp.h — MI/MD contract for symmetric multiprocessing.
 *
 * MI code (the BKL, the scheduler, TLB-shootdown callers) reaches the machine-dependent SMP
 * facilities — CPU enumeration, application-processor bring-up, inter-processor interrupts —
 * only through this contract. The x86_64 impl lives in arch/x86_64/cpu/smp_x86_64.cpp (real
 * INIT-SIPI-SIPI bring-up); i686 keeps a uniprocessor stub (arch/x86/cpu/smp_x86.cpp) so the
 * frozen 32-bit build still links: 1 CPU, no-op IPI.
 */
#pragma once
#include <stdint.h>

namespace arch {

// A function each application processor runs once it is in long mode on the kernel CR3.
typedef void (*ApEntry)();

int  smpCpuCount();   // 1 until smpBringUpAPs() has run; then the number of online CPUs.
int  smpThisCpu();    // dense 0..N-1 index of the calling CPU.

// INIT-SIPI-SIPI every non-BSP LAPIC id in `lapicIds[0..n)`; blocks until each is online.
// Must be called after the LAPIC + per-CPU block (CPU 0) are up. `n` <= MAX_CPUS.
void smpBringUpAPs(const uint8_t* lapicIds, int n);

// Register what an AP runs after bring-up (Phase 3 points this at the scheduler idle loop).
// Set it BEFORE smpBringUpAPs(); unset = APs simply idle (hlt).
void smpSetApEntry(ApEntry fn);

void smpSendIpi(int cpu, uint8_t vector);   // a fixed IPI to one CPU (dense index).
void smpTlbShootdown(uint64_t cr3);          // Phase 4: cross-CPU TLB invalidation.

}  // namespace arch
