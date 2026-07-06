/*
 * tests/lkpi_host_stubs.cpp — host-doctest stubs for the LinuxKPI helpers that the *tested* shim
 * modules reference but whose real definitions live in modules the host build deliberately excludes.
 *
 * The host suite compiles a curated subset of the shim as C++ (see TEST_MODULES): the pure primitives
 * kpi_time / kpi_irq / kpi_kthread are in; kpi_fence.c and kpi_misc.c are NOT — they drag in the whole
 * dma-fence / mm / shmem shim (struct dma_fence, alloc_pages_exact, …) which has no host backing. Those
 * excluded modules define:
 *   - lkpi_wait_pump      (kpi_fence.c)  — the cooperative wait pump
 *   - lkpi_spin_probe     (kpi_misc.c)  — the >2 s starved-spin watchdog
 *   - lkpi_stack_baseline / lkpi_stack_deep / lkpi_deep_report (kpi_misc.c) — the stack tripwire
 * The tested modules call them (msleep/usleep pump, flush_work pump, the queue_work/irq tripwires), so
 * the host link needs definitions. Inert no-ops are correct here: there is no real workqueue/timer/GPU
 * to pump on the host, and the tripwire never fires (stack_deep == 0 == "not deep").
 *
 * Linkage MUST match the declarations the tested modules see, or the symbols stay unresolved:
 *   - lkpi_stack_* / lkpi_deep_report are declared extern "C" (linuxkpi/lkpi_knx.h) → define extern "C".
 *   - lkpi_wait_pump / lkpi_spin_probe are declared plain in <linux/wait.h>/<linux/wait_bit.h> (so a C++
 *     TU never sees two different-linkage decls) → define them plain (C++-mangled), matching the refs.
 */

extern "C" {
	void lkpi_stack_baseline(void) {}
	int  lkpi_stack_deep(void) { return 0; }
	void lkpi_deep_report(const char *where, void *ra) { (void)where; (void)ra; }
}

void lkpi_wait_pump(void) {}
void lkpi_spin_probe(void *ra) { (void)ra; }
