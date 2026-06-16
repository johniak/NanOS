/*
 * pthread_key_create.c — NanOS adaptation of musl 1.2.5 src/thread/pthread_key_create.c.
 *
 * VERBATIM: __pthread_key_create, __pthread_tsd_size, __pthread_tsd_main, the keys[] table,
 * next_key, the key_lock rwlock, and the main-thread lazy tsd bootstrap
 *     if (!self->tsd) self->tsd = __pthread_tsd_main;
 * The last line is what makes pthread_getspecific/setspecific work on the MAIN thread: its
 * TCB (user/libc-glue/tls.c __nx_main_tcb) is zero-initialised, so self->tsd is NULL until
 * the first key_create points it at __pthread_tsd_main[PTHREAD_KEYS_MAX]. Created threads get
 * a real per-thread tsd[] array carved out of their stack mapping in pthread_create.c.
 *
 * ADAPTED — two pieces of musl's key machinery need cross-thread signalling NanOS does not
 * have yet (Phase 5):
 *   - The `#include "fork_impl.h"` + __pthread_key_atfork() are dropped: they only exist to
 *     drain key_lock across fork(); pthread_atfork is not part of the Phase-4 surface.
 *   - __pthread_key_delete is SIMPLIFIED. Upstream walks the global thread list (td->next)
 *     under __block_app_sigs/__tl_lock to zero EVERY thread's tsd[k]; NanOS's pthread_create
 *     keeps no such thread list and has no signal blocking, so we only clear the calling
 *     thread's slot and free the key. A stale tsd[k] in another thread is never DEREFERENCED
 *     by the runtime (only read back by that thread's getspecific), so this cannot crash; but
 *     nothing clears a slot "on use", so if slot k is later reused by a new key_create, a
 *     thread that had setspecific(k,...) before the delete would observe the stale value from
 *     its new key until it overwrites it. Full cross-thread clearing + destructor-on-delete is
 *     deferred to Phase 5 (delete-then-recreate across threads is rare).
 * __pthread_tsd_run_dtors (per-thread-exit destructor sweep) is kept verbatim but is NOT
 * wired into __pthread_exit yet — TSD destructors at thread exit are likewise deferred.
 * __tl_lock/__tl_unlock are weak-aliased to a no-op here exactly as upstream does.
 */
#include "pthread_impl.h"

volatile size_t __pthread_tsd_size = sizeof(void *) * PTHREAD_KEYS_MAX;
void *__pthread_tsd_main[PTHREAD_KEYS_MAX] = { 0 };

static void (*keys[PTHREAD_KEYS_MAX])(void *);

static pthread_rwlock_t key_lock = PTHREAD_RWLOCK_INITIALIZER;

static pthread_key_t next_key;

static void nodtor(void *dummy)
{
}

static void dummy_0(void)
{
}

weak_alias(dummy_0, __tl_lock);
weak_alias(dummy_0, __tl_unlock);

int __pthread_key_create(pthread_key_t *k, void (*dtor)(void *))
{
	pthread_t self = __pthread_self();

	/* This can only happen in the main thread before
	 * pthread_create has been called. */
	if (!self->tsd) self->tsd = __pthread_tsd_main;

	/* Purely a sentinel value since null means slot is free. */
	if (!dtor) dtor = nodtor;

	__pthread_rwlock_wrlock(&key_lock);
	pthread_key_t j = next_key;
	do {
		if (!keys[j]) {
			keys[next_key = *k = j] = dtor;
			__pthread_rwlock_unlock(&key_lock);
			return 0;
		}
	} while ((j=(j+1)%PTHREAD_KEYS_MAX) != next_key);

	__pthread_rwlock_unlock(&key_lock);
	return EAGAIN;
}

int __pthread_key_delete(pthread_key_t k)
{
	/* Simplified: clear only the calling thread's slot and free the key. Cross-thread
	 * tsd[k] clearing (musl's td->next walk under signal-block) needs Phase 5. */
	pthread_t self = __pthread_self();

	__pthread_rwlock_wrlock(&key_lock);

	if (self->tsd) self->tsd[k] = 0;
	keys[k] = 0;

	__pthread_rwlock_unlock(&key_lock);

	return 0;
}

void __pthread_tsd_run_dtors()
{
	pthread_t self = __pthread_self();
	int i, j;
	for (j=0; self->tsd_used && j<PTHREAD_DESTRUCTOR_ITERATIONS; j++) {
		__pthread_rwlock_rdlock(&key_lock);
		self->tsd_used = 0;
		for (i=0; i<PTHREAD_KEYS_MAX; i++) {
			void *val = self->tsd[i];
			void (*dtor)(void *) = keys[i];
			self->tsd[i] = 0;
			if (val && dtor && dtor != nodtor) {
				__pthread_rwlock_unlock(&key_lock);
				dtor(val);
				__pthread_rwlock_rdlock(&key_lock);
			}
		}
		__pthread_rwlock_unlock(&key_lock);
	}
}

weak_alias(__pthread_key_create, pthread_key_create);
weak_alias(__pthread_key_delete, pthread_key_delete);
