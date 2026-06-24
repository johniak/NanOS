#pragma once
#include <stdint.h>
#include <arch/cpu.h>   // cpuIrqSave/cpuIrqRestore for the IRQ-save guard
#include <arch/smp.h>   // smpThisCpu() for the recursive (per-CPU keyed) lock

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
			arch::cpuRelax();   // PAUSE hint via the arch contract (keeps this header MI)
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

// Plain RAII guard (no interrupt toggling) — for a lock taken only from thread context, never an
// IRQ handler, where the critical section may be long (e.g. block-cache device I/O) and disabling
// IRQs would starve the BSP timer. Mutual exclusion across CPUs holds; IRQ-context use is unsafe.
class SpinGuard {
	Spinlock& l;
public:
	explicit SpinGuard(Spinlock& s) : l(s) { l.lock(); }
	~SpinGuard() { l.unlock(); }
	SpinGuard(const SpinGuard&) = delete;
	SpinGuard& operator=(const SpinGuard&) = delete;
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

// A RECURSIVE spinlock keyed on the owning CPU (same shape as the Big Kernel Lock, Bkl.h, but
// reusable for a subsystem that composes its own locked methods). The win over a plain Spinlock:
// a public op that calls another public op of the same subsystem (e.g. ProcTable::infoByPid ->
// byPid) re-enters on the SAME CPU via depth++ instead of self-deadlocking; a DIFFERENT CPU still
// blocks. ownerCpu/depth are only touched by the holder, so they need no separate atomic.
class RecursiveSpinlock {
	Spinlock     lock;
	volatile int ownerCpu = -1;   // dense CPU index holding it, or -1
	volatile int depth    = 0;    // recursion depth on the owning CPU
public:
	void enter() {
		int cpu = arch::smpThisCpu();
		if (ownerCpu == cpu) { depth++; return; }   // already ours -> nest
		lock.lock();                                // blocks until the holding CPU releases
		ownerCpu = cpu;
		depth = 1;
	}
	void exit() {
		if (--depth == 0) { ownerCpu = -1; lock.unlock(); }
	}
};

// RAII guard for RecursiveSpinlock that ALSO disables local interrupts for the whole (possibly
// nested) critical section — so an IRQ handler that takes the same lock cannot fire mid-update on
// this CPU. Each guard saves/restores the flags: the innermost restores first (back to "disabled"),
// the outermost restores last (to the caller's original state), while enter/exit ref-count the
// actual unlock. Safe to nest; safe to take from both thread and IRQ context.
class RecursiveIrqGuard {
	RecursiveSpinlock& l;
	unsigned long flags;
public:
	explicit RecursiveIrqGuard(RecursiveSpinlock& s) : l(s) { flags = arch::cpuIrqSave(); l.enter(); }
	~RecursiveIrqGuard() { l.exit(); arch::cpuIrqRestore(flags); }
	RecursiveIrqGuard(const RecursiveIrqGuard&) = delete;
	RecursiveIrqGuard& operator=(const RecursiveIrqGuard&) = delete;
};

// RAII guard for RecursiveSpinlock that does NOT touch the interrupt flag — for a subsystem taken
// only from thread (syscall) context, never an IRQ handler, where the critical section can be long
// (e.g. the VFS holds it across polling disk I/O) and disabling IRQs would starve the BSP timer.
// Mutual exclusion across CPUs + same-CPU recursion still hold; only IRQ-context use is unsafe.
class RecursiveGuard {
	RecursiveSpinlock& l;
public:
	explicit RecursiveGuard(RecursiveSpinlock& s) : l(s) { l.enter(); }
	~RecursiveGuard() { l.exit(); }
	RecursiveGuard(const RecursiveGuard&) = delete;
	RecursiveGuard& operator=(const RecursiveGuard&) = delete;
};

}  // namespace kernel
