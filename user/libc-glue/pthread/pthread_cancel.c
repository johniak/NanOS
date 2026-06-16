/*
 * pthread_cancel.c — request cancellation of a target thread (NanOS deferred model).
 *
 * Set the target's cancel flag, THEN interrupt it with SIGCANCEL so that if it is parked in a
 * cancellable futex (pthread_cond_wait / sem_wait) it wakes with -EINTR and acts on the flag at
 * its cancellation point (see cancel_impl.c + (__syscall_cp) in nanos_glue.c). The order
 * matters: the flag must be visible before the wake, or the woken thread could re-check and
 * re-block without seeing the request.
 *
 * SIGCANCEL is the kernel's value (32, kernel/Signal.h), NOT musl's pthread_impl.h SIGCANCEL
 * (33) — NanOS reserves the first RT signal for this. SYS_tkill (238) targets a specific tid
 * and needs no tgid; the NanOS kernel allows SIGCANCEL via tkill as the in-process cancel
 * transport (kill-by-pid still refuses it).
 */
#include "pthread_impl.h"
#include "syscall.h"

/* Kernel SIGCANCEL (kernel/Signal.h). Distinct from musl's pthread_impl.h SIGCANCEL (33). */
#define NX_SIGCANCEL 32

int pthread_cancel(pthread_t t)
{
	a_store(&t->cancel, 1);
	/* POSIX: ESRCH if no such thread. tkill returns -ESRCH (negated errno); map it. */
	int r = __syscall(SYS_tkill, t->tid, NX_SIGCANCEL);
	return r < 0 ? -r : 0;
}
