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
