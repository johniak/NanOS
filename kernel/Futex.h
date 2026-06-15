/*
 * Futex.h — the pure futex wait-bucket table (machine-independent, host-tested).
 *
 * A futex is identified NOT by a bare user virtual address (two different address
 * spaces can both have a futex at VA 0x1000) but by the pair (AddressSpace*, uaddr):
 * the `space` pointer scopes the key to one process, since every thread of a process
 * shares one AddressSpace. So a private futex's waiters never collide with another
 * process's waiters at the same VA.
 *
 * FutexTable hashes (space, uaddr) -> a fixed bucket array; each bucket is an intrusive
 * FIFO of FutexWaiter nodes. There is NO dynamic allocation here: each waiter supplies
 * its own FutexWaiter (it lives on the blocking thread's kernel stack); the table only
 * links/unlinks them. Wake order is FIFO (oldest waiter first), as Linux does.
 *
 * This file is deliberately free of scheduler/arch dependencies: the table never
 * dereferences Task* (a forward decl suffices), so the whole structure runs natively
 * under the host doctest suite. The kernel glue (block the current task, call
 * Scheduler::wake on each woken waiter) lives outside, on top of this table.
 */
#ifndef FUTEX_H_
#define FUTEX_H_

namespace kernel {

struct Task;   // scheduler task (Scheduler.h) — only ever stored, never dereferenced here

// One parked waiter. Allocated by the caller (on its kernel stack); the table links it
// into a bucket FIFO. `space`/`uaddr` are filled in by enqueue (they are the key).
struct FutexWaiter {
	const void* space;     // owning AddressSpace* (key part 1)
	void*       uaddr;      // user virtual address of the futex word (key part 2)
	Task*       task;       // the blocked task to wake (opaque to this table)
	// FUTEX_WAIT_BITSET mask. A plain FUTEX_WAIT waiter stores 0 here, meaning "wake me only
	// via wake(), not wakeBitset()". wakeBitset matches (w->bitset & mask) != 0, so a bitset==0
	// waiter is NEVER matched by it — not even with mask == ~0u (FUTEX_BITSET_MATCH_ANY). The
	// kernel glue (Task 1.2) must therefore use wake() for FUTEX_WAKE and reserve wakeBitset()
	// for FUTEX_WAKE_BITSET with an explicit caller-supplied mask.
	unsigned    bitset;
	FutexWaiter* next;      // intrusive FIFO link within a bucket
};

class FutexTable {
public:
	FutexTable();

	// Append `w` to the FIFO for key (space, uaddr), stamping w->space/w->uaddr.
	void enqueue(const void* space, void* uaddr, FutexWaiter* w);

	// Remove up to `n` waiters matching (space, uaddr) in FIFO order; return how many
	// were removed. The caller wakes each (this pure table does not touch the scheduler).
	int wake(const void* space, void* uaddr, int n);

	// Like wake, but only matches waiters whose (bitset & arg) != 0. NOTE: a waiter parked
	// with bitset==0 (a plain FUTEX_WAIT) is never matched here — use wake() for FUTEX_WAKE.
	int wakeBitset(const void* space, void* uaddr, int n, unsigned bitset);

	// Wake up to `nwake` waiters on (fromSpace, from), then move up to `nmove` of the
	// remaining from-waiters to (toSpace, to), re-keying them. Returns nwoken + nmoved.
	int requeue(const void* fromSpace, void* from,
	            const void* toSpace, void* to, int nwake, int nmove);

	// Number of waiters currently parked on (space, uaddr).
	int count(const void* space, void* uaddr) const;

	// Unlink a specific waiter wherever it sits (no-op if not linked). Used by the kernel
	// glue to cancel a waiter on timeout or signal.
	void remove(FutexWaiter* w);

private:
	static const int NBUCKETS = 64;   // power of two
	FutexWaiter* buckets_[NBUCKETS];

	static int hash(const void* space, const void* uaddr);
};

}  // namespace kernel

#endif /* FUTEX_H_ */
