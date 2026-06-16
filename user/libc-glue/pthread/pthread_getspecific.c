/*
 * pthread_getspecific.c — musl 1.2.5 src/thread/pthread_getspecific.c.
 *
 * Verbatim except the `#include <threads.h>` and the C11 `tss_get` weak_alias are dropped:
 * NanOS has no <threads.h> (C11 threads are out of scope for Phase 4), and the POSIX
 * pthread_getspecific name is the only one the test/programs use. The body — read the
 * calling thread's own tsd[] slot via %gs:0 self — is unchanged.
 */
#include "pthread_impl.h"

static void *__pthread_getspecific(pthread_key_t k)
{
	struct pthread *self = __pthread_self();
	return self->tsd[k];
}

weak_alias(__pthread_getspecific, pthread_getspecific);
