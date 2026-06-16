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
 * __cancel(), which never returns — it exits the thread with PTHREAD_CANCELED, running cleanup
 * handlers on the way out (in __pthread_exit).
 *
 * LIMITATION: PTHREAD_CANCEL_ASYNCHRONOUS is best-effort at cancellation-point granularity.
 * Without PC inspection we cannot preempt a thread mid-computation; an async-cancellable thread
 * is still only cancelled when it next reaches a cancellation point (or is interrupted in one).
 */
#include "pthread_impl.h"
#include "pthread.h"

_Noreturn void __pthread_exit(void *result);   /* defined in pthread_create.c */

/* The cancellation action. Disable further cancellation (we are already leaving), record the
 * canonical PTHREAD_CANCELED result, and exit — __pthread_exit walks the cleanup chain and then
 * SYS_exit(0)s, so the joiner observes self->result == PTHREAD_CANCELED. Never returns. */
_Noreturn void __cancel(void)
{
	pthread_t self = __pthread_self();
	self->canceldisable = 1;
	self->cancelasync = 0;
	self->result = PTHREAD_CANCELED;
	__pthread_exit(PTHREAD_CANCELED);
}
