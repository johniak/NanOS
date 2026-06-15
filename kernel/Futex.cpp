/*
 * Futex.cpp — pure futex wait-bucket table. See Futex.h for the design.
 *
 * Each bucket is the head of an intrusive singly-linked FIFO of FutexWaiter nodes,
 * appended at the tail (walk to find it — buckets are short) so wakes happen in
 * arrival order. No allocation: the table only relinks caller-owned nodes.
 */
#include "Futex.h"

namespace kernel {

FutexTable::FutexTable() {
	for (int i = 0; i < NBUCKETS; i++) buckets_[i] = 0;
}

// Mix the two pointer values and fold to the bucket count. A trivial xor+shift hash
// is plenty: collisions only mean two keys share a bucket, which the per-node key
// comparison disambiguates.
int FutexTable::hash(const void* space, const void* uaddr) {
	unsigned long s = (unsigned long)space;
	unsigned long a = (unsigned long)uaddr;
	unsigned long h = (s >> 4) ^ (a >> 4) ^ (s >> 12) ^ (a >> 16);
	return (int)(h & (NBUCKETS - 1));
}

void FutexTable::enqueue(const void* space, void* uaddr, FutexWaiter* w) {
	if (!w) return;
	w->space = space;
	w->uaddr = uaddr;
	w->next = 0;
	FutexWaiter** pp = &buckets_[hash(space, uaddr)];
	while (*pp) pp = &(*pp)->next;   // walk to the tail
	*pp = w;
}

int FutexTable::count(const void* space, void* uaddr) const {
	int n = 0;
	for (FutexWaiter* w = buckets_[hash(space, uaddr)]; w; w = w->next)
		if (w->space == space && w->uaddr == uaddr) n++;
	return n;
}

FutexWaiter* FutexTable::popOne(const void* space, void* uaddr) {
	FutexWaiter** pp = &buckets_[hash(space, uaddr)];
	while (*pp) {
		FutexWaiter* w = *pp;
		if (w->space == space && w->uaddr == uaddr) {
			*pp = w->next;       // unlink
			w->next = 0;
			return w;
		}
		pp = &w->next;
	}
	return 0;
}

FutexWaiter* FutexTable::popOneBitset(const void* space, void* uaddr, unsigned bitset) {
	FutexWaiter** pp = &buckets_[hash(space, uaddr)];
	while (*pp) {
		FutexWaiter* w = *pp;
		if (w->space == space && w->uaddr == uaddr && (w->bitset & bitset)) {
			*pp = w->next;       // unlink
			w->next = 0;
			return w;
		}
		pp = &w->next;
	}
	return 0;
}

// wake/wakeBitset are popOne in a loop, discarding the nodes — the count is all the pure table
// reports. The kernel glue uses popOne directly when it needs the Task* to actually wake.
int FutexTable::wake(const void* space, void* uaddr, int n) {
	int woken = 0;
	while (woken < n && popOne(space, uaddr)) woken++;
	return woken;
}

int FutexTable::wakeBitset(const void* space, void* uaddr, int n, unsigned bitset) {
	int woken = 0;
	while (woken < n && popOneBitset(space, uaddr, bitset)) woken++;
	return woken;
}

int FutexTable::requeue(const void* fromSpace, void* from,
                        const void* toSpace, void* to, int nwake, int nmove) {
	int woken = wake(fromSpace, from, nwake);
	if (nmove <= 0) return woken;

	int moved = 0;
	FutexWaiter** pp = &buckets_[hash(fromSpace, from)];
	while (*pp && moved < nmove) {
		FutexWaiter* w = *pp;
		if (w->space == fromSpace && w->uaddr == from) {
			*pp = w->next;                 // unlink from the source bucket
			w->space = toSpace;            // re-key
			w->uaddr = to;
			w->next = 0;
			FutexWaiter** tp = &buckets_[hash(toSpace, to)];
			while (*tp) tp = &(*tp)->next; // append to the dest tail
			*tp = w;
			moved++;
		} else {
			pp = &w->next;
		}
	}
	return woken + moved;
}

int futexWaitPrecheck(const unsigned* uaddr, unsigned expected) {
	return (*uaddr == expected) ? 0 : -11;   // -EAGAIN: the word changed under us, don't park
}

void FutexTable::remove(FutexWaiter* w) {
	if (!w) return;
	FutexWaiter** pp = &buckets_[hash(w->space, w->uaddr)];
	while (*pp) {
		if (*pp == w) { *pp = w->next; w->next = 0; return; }
		pp = &(*pp)->next;
	}
}

}  // namespace kernel
