#pragma once
#include <stdint.h>

namespace arch {

// Per-CPU control block. One per logical CPU; IA32_KERNEL_GS_BASE on each CPU points at its
// own block, so the SYSCALL/IRQ entry stubs reach CPU-local state after `swapgs`.
//
// The first two fields KEEP their offsets — syscall_entry64.S hard-codes %gs:0 (kernel stack
// top) and %gs:8 (user RSP scratch). Do not reorder them without updating that .S.
struct PerCpu {
	uint64_t kernelStackTop;   // [0]  SYSCALL stub loads this after swapgs  (OFFSET FIXED)
	uint64_t userRspScratch;   // [8]  SYSCALL stub scratch                  (OFFSET FIXED)
	uint32_t cpuIndex;         // [16] dense 0..N-1 index (NOT the LAPIC id)
	uint32_t lapicId;          // [20]
	void*    currentTask;      // [24] kernel::Task* running on THIS cpu (Phase 3)
	void*    currentThread;    // [32]
	uint32_t inIrq;            // [40] interrupt nesting depth on this cpu
};

static const int MAX_CPUS = 32;
extern PerCpu g_percpu[MAX_CPUS];          // gs base on cpu i = &g_percpu[i]

// Set up the calling CPU's per-CPU block + point its GS base at it (kernel-only; uses wrmsr).
void perCpuInitThis(uint32_t idx, uint32_t lapicId, uint64_t kernelStackTop);

// Pure helper (host-testable): map a LAPIC id back to its dense index using the id table the
// bring-up code built, or -1 if absent. Used by an AP to find its own index at startup.
static inline int cpuIndexForLapic(const uint8_t* ids, int n, uint8_t lapicId) {
	for (int i = 0; i < n; i++) if (ids[i] == lapicId) return i;
	return -1;
}

}  // namespace arch
