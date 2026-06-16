/*
 * nanos_glue.c — the thin NanOS adapter layer the vendored musl pthread internals link
 * against. It supplies the few externals the verbatim musl sources reference but that NanOS
 * does not yet have a full implementation for. Kept tiny and isolated so the vendored .c/.s
 * files stay unmodified.
 *
 *   __libc                  — musl's global libc state (libc.h). The pthread core reads
 *                             libc.need_locks (in __lock.c). Zero-initialised => single-
 *                             threaded fast path until pthread_create flips it (Task 4.2).
 *   __syscall_cp(...)       — musl routes cancellable syscalls through this. NanOS honours a
 *                             pending cancel here (deferred model, Task 5.3): check the flag
 *                             before the call and on -EINTR after it; otherwise plain syscall.
 *   __clock_gettime(...)    — musl's internal clock hook; forward to picolibc clock_gettime
 *                             (NanOS impl in user/libc-glue/syscalls.c).
 *   __pthread_setcancelstate— real cancellation enable/disable (+ setcanceltype, testcancel).
 *                             __timedwait brackets the wait with DISABLE so locks never cancel.
 */
#include <time.h>
#include <stddef.h>
#include <errno.h>
#include "libc.h"
#include "syscall.h"
#include "pthread.h"
#include "pthread_impl.h"

/* errno coherence is the project's MAIN integration risk: picolibc's __errno_location
 * (user/libc-glue/tls.c) returns the cell at %gs+28 (nx-tcb.h's struct __pthread::__errno),
 * and the i386 thread pointer is the TCB base (TP_ADJ identity, %gs:0 = self). musl's
 * struct pthread MUST agree — self@0, tid@24, errno_val@28 — or musl and picolibc would see
 * DIFFERENT errno cells. Lock it at compile time: a musl bump, or a TLS_ABOVE_TP / CANARY_PAD
 * config that shifted these, fails the build here instead of silently desyncing errno. */
_Static_assert(offsetof(struct pthread, self) == 0,        "TCB self must be at %gs:0");
_Static_assert(offsetof(struct pthread, tid) == 24,        "TCB tid offset drifted from nx-tcb.h");
_Static_assert(offsetof(struct pthread, errno_val) == 28,  "musl errno_val must match picolibc errno at %gs+28");

/* musl's process-global libc state. Hidden to match the `extern hidden` decl in libc.h. */
struct __libc __libc;

/* The cancellation action (cancel_impl.c) — never returns; exits the thread with
 * PTHREAD_CANCELED, running cleanup handlers via __pthread_exit. */
_Noreturn void __cancel(void);

/* Cancellable-syscall entry point — the heart of the NanOS deferred-cancellation model.
 * A pending cancel (flag set + not disabled) is honoured at the cancellation point: BEFORE the
 * call (so a request that arrived while runnable cancels here) and, if the syscall came back
 * -EINTR, AFTER it (the SIGCANCEL that pthread_cancel sent interrupted a blocked futex). In
 * both cases __cancel() does not return. Otherwise it is a plain 6-arg int $0x80, returning the
 * kernel's negated-errno result. A thread with cancellation disabled just runs the syscall. */
/* Parenthesised name dodges the __syscall_cp(...) dispatch macro from syscall.h, exactly as
 * musl's own src/thread/__syscall_cp.c defines `long (__syscall_cp)(...)`. */
hidden long (__syscall_cp)(syscall_arg_t n, syscall_arg_t a, syscall_arg_t b,
                           syscall_arg_t c, syscall_arg_t d, syscall_arg_t e, syscall_arg_t f)
{
	pthread_t self = __pthread_self();
	if (self->cancel && !self->canceldisable)
		__cancel();                                  /* pending cancel at this cp */
	long r = __syscall6(n, a, b, c, d, e, f);
	if (r == -EINTR && self->cancel && !self->canceldisable)
		__cancel();                                  /* interrupted by SIGCANCEL while blocked */
	return r;
}

/* Translate a raw kernel return into the userland (val, errno) convention. musl's syscall()
 * wrapper macro (syscall.h) expands to __syscall_ret(__syscall(...)); pthread_create/join reach
 * it via the syscall() form. Same body as musl's src/internal/syscall_ret.c, except errno is the
 * per-thread cell (picolibc's __errno_location -> %gs:0 -> self->errno). */
hidden long __syscall_ret(unsigned long r)
{
	if (r > -4096UL) {
		errno = -(long)r;
		return -1;
	}
	return (long)r;
}

/* musl's internal monotonic/realtime clock hook, used by __timedwait to compute timeouts. */
hidden int __clock_gettime(clockid_t clk, struct timespec *ts)
{
	return clock_gettime(clk, ts);
}

/* pthread_setcancelstate(state, oldstate): enable/disable cancellation for the calling thread.
 * NanOS stores canceldisable as a strict boolean (DISABLE => 1, else 0); PTHREAD_CANCEL_MASKED
 * (used internally by pthread_cond_timedwait) is therefore treated as ENABLE — cond/sem waits
 * are cancellation points. Validates state against the three legal values, reports the prior
 * state (DISABLE/ENABLE), and succeeds. __timedwait (the NON-cancellable wait used by mutex/
 * rwlock locks) brackets its futex wait with DISABLE..restore, so those waits never cancel. */
hidden int __pthread_setcancelstate(int state, int *oldstate)
{
	if ((unsigned) state > 2u)
		return EINVAL;   /* ENABLE(0) / DISABLE(1) / MASKED(2) */
	pthread_t self = __pthread_self();
	if (oldstate)
		*oldstate = self->canceldisable ? PTHREAD_CANCEL_DISABLE : PTHREAD_CANCEL_ENABLE;
	self->canceldisable = (state == PTHREAD_CANCEL_DISABLE);
	return 0;
}
weak_alias(__pthread_setcancelstate, pthread_setcancelstate);

/* pthread_setcanceltype(type, oldtype): DEFERRED(0) vs ASYNCHRONOUS(1). NanOS has no PC
 * inspection, so ASYNCHRONOUS is best-effort at cancellation-point granularity (it cannot
 * preempt a thread mid-computation); we still record it so __cancel/self-cancel semantics that
 * key off cancelasync behave. */
int pthread_setcanceltype(int type, int *oldtype)
{
	if ((unsigned) type > 1u)
		return EINVAL;   /* DEFERRED(0) / ASYNCHRONOUS(1) */
	pthread_t self = __pthread_self();
	if (oldtype)
		*oldtype = self->cancelasync;
	self->cancelasync = (unsigned char) type;
	return 0;
}

/* Cancellation point hook. pthread_cond_timedwait() calls __pthread_testcancel() on entry and
 * after a consumed signal; it now acts on a pending request (flag set + not disabled) by
 * running __cancel() (which does not return). weak_alias to the public name so a program
 * calling pthread_testcancel() links too. */
hidden void __pthread_testcancel(void)
{
	pthread_t self = __pthread_self();
	if (self->cancel && !self->canceldisable)
		__cancel();
}
weak_alias(__pthread_testcancel, pthread_testcancel);
