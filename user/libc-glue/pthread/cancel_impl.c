/*
 * cancel_impl.c — the shared cancellation ACTION for the NanOS deferred-cancellation model.
 *
 * NanOS does NOT use musl's upstream cancellation machinery (the SA_SIGINFO handler that
 * inspects/rewrites the interrupted PC via ucontext, the __cp_begin/__cp_end window in
 * __syscall_cp.s). The NanOS signal frame only supports a simple `void handler(int)` — there
 * is no SA_SIGINFO/ucontext/PC-inspection. So cancellation is DEFERRED and driven by a flag +
 * futex-EINTR: a cancellable syscall (__syscall_cp) checks self->cancel before and after the
 * call; pthread_cancel sets the flag and pokes the target thread with SIGCANCEL so a blocked
 * futex returns -EINTR (SIGCANCEL is forced non-restarting in the kernel). The check then runs
 * __cancel(), which DECIDES what to do based on the current cancel-enable state (mirroring
 * musl's src/thread/cancel_impl.c):
 *   - ENABLE (a real cancellation point with cancellation enabled) or ASYNCHRONOUS:
 *       exit the thread now with PTHREAD_CANCELED, running cleanup handlers via __pthread_exit.
 *   - MASKED / DISABLED-but-pending (deferred): do NOT exit. Record the pending cancellation
 *       (self->canceled = 1) and return -ECANCELED so the caller (e.g. pthread_cond_timedwait,
 *       which masks cancellation around its futex wait) can unwind first — unlink its stack
 *       waiter node from the condvar, relock the mutex — and only THEN re-test for cancellation
 *       (at its `done:` label, once the real ENABLE/DISABLE state is restored). This is what
 *       prevents a cancelled cond-waiter from leaving a dead node linked on the condvar.
 *
 * LIMITATION: PTHREAD_CANCEL_ASYNCHRONOUS is best-effort at cancellation-point granularity.
 * Without PC inspection we cannot preempt a thread mid-computation; an async-cancellable thread
 * is still only cancelled when it next reaches a cancellation point (or is interrupted in one).
 */
#include <errno.h>
#include "pthread_impl.h"
#include "pthread.h"

_Noreturn void __pthread_exit(void *result);   /* defined in pthread_create.c */

/* The cancellation action. Returns -ECANCELED in the masked/deferred case (the caller unwinds
 * and re-tests later); only the enabled/async path is _Noreturn (it exits via __pthread_exit,
 * which walks the cleanup chain and SYS_exit(0)s, so the joiner observes PTHREAD_CANCELED). */
long __cancel(void)
{
	pthread_t self = __pthread_self();
	if (self->canceldisable == PTHREAD_CANCEL_ENABLE || self->cancelasync)
		__pthread_exit(PTHREAD_CANCELED);
	self->canceled = 1;
	return -ECANCELED;
}
