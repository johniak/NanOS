/*
 * pthread_detach.c — NanOS adaptation of musl 1.2.5 src/thread/pthread_detach.c.
 *
 * Records DT_DETACHED so no one waits on the thread. On NanOS a thread frees no resources of
 * its own at exit (munmap is a no-op; the stack/TCB are reclaimed at process exit), so detach
 * has nothing to munmap — it is purely a state transition. Trimmed of musl's block_app_sigs /
 * __unmapself / DT_EXITING race handling (Phase 5 / not needed without a real unmap path).
 */
#include "pthread_impl.h"

int __pthread_detach(pthread_t t)
{
	/* JOINABLE -> DETACHED if the thread is still running. If it has already exited (state is
	 * DT_EXITED) there is nothing to reclaim on NanOS, so succeed regardless. */
	a_cas(&t->detach_state, DT_JOINABLE, DT_DETACHED);
	return 0;
}
weak_alias(__pthread_detach, pthread_detach);
