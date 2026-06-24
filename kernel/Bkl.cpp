/*
 * Bkl.cpp — the Big Kernel Lock, RETIRED in Phase 4, Task 15f.
 *
 * Through Phase 3 + Tasks 11-15e a single BKL serialized all kernel execution while fine-grained
 * locks were layered underneath every shared subsystem: the runqueue (g_rqLock), the process table
 * (g_procLock), VFS (g_vfsLock), block cache + JBD2, the frame allocator + heap, the fd table, pipes,
 * the futex table, the exec staging window, the console, and the network stack (g_netLock). With all
 * of those in place — and validated by the smptorture + nettorture data-race gates — the global lock
 * is no longer needed for correctness.
 *
 * bklEnter/bklExit are now NO-OPS so the SYSCALL/IRQ/ISR entry stubs + the one-way ring-3 entry still
 * link and run unchanged (no asm edits in the flip), simply without taking a global lock; the
 * scheduler's direct g_bkl calls were removed (kernel/Scheduler.cpp). g_bkl is kept (unreferenced)
 * for one release so the change is trivially reversible. The empty-call overhead on the syscall/IRQ
 * path is removed in a follow-up that strips the calls from the asm stubs.
 */
#include "Bkl.h"

namespace kernel {
Bkl g_bkl;   // retained, no longer acquired (see above)
}

extern "C" void bklEnter() { /* BKL retired (Task 15f): no-op */ }
extern "C" void bklExit()  { /* BKL retired (Task 15f): no-op */ }
