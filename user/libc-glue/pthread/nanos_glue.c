/*
 * nanos_glue.c — the thin NanOS adapter layer the vendored musl pthread internals link
 * against. It supplies the few externals the verbatim musl sources reference but that NanOS
 * does not yet have a full implementation for. Kept tiny and isolated so the vendored .c/.s
 * files stay unmodified.
 *
 *   __libc                  — musl's global libc state (libc.h). The pthread core reads
 *                             libc.need_locks (in __lock.c). Zero-initialised => single-
 *                             threaded fast path until pthread_create flips it (Task 4.2).
 *   __syscall_cp(...)       — musl routes cancellable syscalls through this. NanOS has no
 *                             pthread cancellation yet (Phase 5), so it is a plain syscall.
 *   __clock_gettime(...)    — musl's internal clock hook; forward to picolibc clock_gettime
 *                             (NanOS impl in user/libc-glue/syscalls.c).
 *   __pthread_setcancelstate— cancellation stub (no-op) until Phase 5; __timedwait calls it
 *                             around the wait. Records the previous state as ENABLE.
 */
#include <time.h>
#include <stddef.h>
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

/* Cancellable-syscall entry point. No cancellation machinery yet => just issue the raw
 * 6-arg syscall via int $0x80 (syscall_arch.h). Returns the kernel's negated-errno result,
 * exactly as the cancellable variant would on a thread with cancellation disabled. */
/* Parenthesised name dodges the __syscall_cp(...) dispatch macro from syscall.h, exactly as
 * musl's own src/thread/__syscall_cp.c defines `long (__syscall_cp)(...)`. */
hidden long (__syscall_cp)(syscall_arg_t n, syscall_arg_t a, syscall_arg_t b,
                           syscall_arg_t c, syscall_arg_t d, syscall_arg_t e, syscall_arg_t f)
{
	return __syscall6(n, a, b, c, d, e, f);
}

/* musl's internal monotonic/realtime clock hook, used by __timedwait to compute timeouts. */
hidden int __clock_gettime(clockid_t clk, struct timespec *ts)
{
	return clock_gettime(clk, ts);
}

/* Cancellation is Phase 5; until then setcancelstate is a no-op that reports ENABLE as the
 * prior state. __timedwait brackets the futex wait with this, so it must exist + succeed. */
hidden int __pthread_setcancelstate(int new_state, int *old_state)
{
	(void) new_state;
	if (old_state)
		*old_state = PTHREAD_CANCEL_ENABLE;
	return 0;
}
