/*
 * Bkl.h — the Big Kernel Lock (MI).
 *
 * Phase 3 of SMP brings up correctness by SERIALIZATION first: one giant lock around all kernel
 * execution, so the many shared singletons (the scheduler runqueue, the heap, the VFS, the block
 * cache, …) need no per-subsystem locking yet. Phase 4 then replaces it with fine-grained locks.
 *
 * It is a RECURSIVE spinlock keyed on the owning CPU: a fault handler nested inside a syscall, or
 * an IRQ that lands on a CPU already in the kernel, re-enters instead of self-deadlocking. The
 * recursion makes the single-CPU path deadlock-proof (the BSP always owns it, never waits on
 * itself), so the lock can be wired in before any AP runs kernel code (Task 8) and verified on a
 * uniprocessor boot.
 *
 * Ownership rule: ownerCpu/depth are only ever touched by the lock holder (or a same-CPU nested
 * enter), so they need no separate atomic — but they MUST be read/written with the lock held or
 * by the owning CPU. arch::smpThisCpu() must be correct in every kernel context (it is: the
 * x86_64 impl maps the LAPIC id, not %gs).
 */
#pragma once
#include "Spinlock.h"
#include <arch/smp.h>

namespace kernel {

struct Bkl {
    Spinlock     lock;
    volatile int ownerCpu = -1;   // dense CPU index currently holding the lock, or -1
    volatile int depth    = 0;    // recursion depth on the owning CPU

    void enter() {
        int cpu = arch::smpThisCpu();
        if (ownerCpu == cpu) {    // already ours -> nest (fault inside syscall, IRQ in kernel)
            depth++;
            return;
        }
        // The ticket-lock spin MUST be uninterruptible: with IF set, a same-CPU IRQ would re-enter
        // enter(), grab a second (later) ticket, and the FIFO lock would self-deadlock — the inner
        // wait can't be served before the outer, which can't run while the inner spins. So disable
        // interrupts across the acquisition, then restore: once ownerCpu==cpu, a nested IRQ takes
        // the recursive depth++ path above and never touches the lock.
        unsigned long flags = arch::cpuIrqSave();
        lock.lock();              // blocks (IRQ-free) until the holding CPU releases
        ownerCpu = cpu;
        depth = 1;
        arch::cpuIrqRestore(flags);
    }

    void exit() {
        if (--depth == 0) {
            ownerCpu = -1;
            lock.unlock();
        }
    }

    // True iff THIS cpu holds the lock (for assertions / conditional release on rare paths).
    bool heldByThisCpu() const { return ownerCpu == arch::smpThisCpu(); }
};

extern Bkl g_bkl;

}  // namespace kernel
