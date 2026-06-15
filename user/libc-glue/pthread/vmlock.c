/*
 * vmlock.c — NanOS adaptation of musl 1.2.5 src/thread/vmlock.c.
 *
 * Verbatim except the `#include "fork_impl.h"` and the `__vmlock_lockptr` export are dropped:
 * those exist only so pthread_atfork()/fork can drain the vm lock across a fork, which is not
 * part of the pthread mutex/cond surface. The three functions are reached only by the
 * process-SHARED, ownership-tracking mutex paths in pthread_mutex_unlock()/destroy(); a
 * process-private mutex (all NanOS supports) never enters them, so this is link-completeness,
 * not a runtime path.
 */
#include "pthread_impl.h"

static volatile int vmlock[2];

void __vm_wait()
{
	int tmp;
	while ((tmp=vmlock[0]))
		__wait(vmlock, vmlock+1, tmp, 1);
}

void __vm_lock()
{
	a_inc(vmlock);
}

void __vm_unlock()
{
	if (a_fetch_add(vmlock, -1)==1 && vmlock[1])
		__wake(vmlock, -1, 1);
}
