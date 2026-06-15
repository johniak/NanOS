/*
 * pthread_join.c — NanOS adaptation of musl 1.2.5 src/thread/pthread_join.c.
 *
 * Trimmed of cancellation (Phase 5) and the timed/tryjoin variants. NanOS's join word is the
 * thread's tid: the kernel zeroes &tcb->tid and futex-wakes it on thread exit (CLONE_CHILD_
 * CLEARTID, Task 2.3), so we simply FUTEX_WAIT on it until it reads 0, then harvest the result.
 * (Upstream musl waits on &t->detach_state and wakes it from userspace; we lean on the kernel's
 * CLEARTID instead — see pthread_create.c.)
 */
#include "pthread_impl.h"
#include <sys/mman.h>
#include <time.h>

int __pthread_join(pthread_t t, void **res)
{
	volatile int *tidp = (volatile int *)&t->tid;
	int tmp;
	/* FUTEX_WAIT returns immediately (-EAGAIN) if *tidp no longer equals the sampled value,
	 * so a thread that exited before we first looked, or a spurious wake, just re-loops. */
	while ((tmp = *tidp))
		__timedwait((volatile int *)tidp, tmp, CLOCK_REALTIME, 0, 1 /* FUTEX_PRIVATE */);

	a_barrier();
	if (res)
		*res = t->result;
	/* No-op on NanOS (no unmap syscall; pages freed at process exit). Kept for code shape /
	 * intent so a real unmap path can slot in later without touching join. */
	if (t->map_base)
		munmap(t->map_base, t->map_size);
	return 0;
}
weak_alias(__pthread_join, pthread_join);
