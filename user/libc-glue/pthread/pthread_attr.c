/*
 * pthread_attr.c — the pthread_attr_t getters/setters, adapted from musl 1.2.5
 * src/thread/pthread_attr_*.c (one TU instead of musl's per-function files). Only the subset
 * pthread_create reads and that real programs commonly set: detachstate, stacksize, guardsize,
 * plus init/destroy. The fields are musl's _a_* aliases over pthread_attr_t (pthread_impl.h).
 * __acquire_ptc/__release_ptc bracketing in upstream init is dropped (no per-thread-create
 * critical section on NanOS yet).
 */
#include "pthread_impl.h"
#include <stdint.h>

#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 2048
#endif

int pthread_attr_init(pthread_attr_t *a)
{
	*a = (pthread_attr_t){ 0 };
	a->_a_stacksize = __default_stacksize;
	a->_a_guardsize = __default_guardsize;
	return 0;
}

int pthread_attr_destroy(pthread_attr_t *a)
{
	(void)a;
	return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
	if ((unsigned)state > 1U)
		return EINVAL;
	a->_a_detach = state;
	return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
	*state = a->_a_detach;
	return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t size)
{
	if (size < PTHREAD_STACK_MIN)
		return EINVAL;
	a->_a_stackaddr = 0;
	a->_a_stacksize = size;
	return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size)
{
	*size = a->_a_stacksize;
	return 0;
}

/* pthread_attr_setstack/getstack — set/read the explicit stack base+size (V8 reads a thread's stack
 * bounds via pthread_getattr_np + pthread_attr_getstack for its overflow checks). Same fields as the
 * *stacksize pair. `stackaddr` is the LOW end of the stack region, per POSIX. */
int pthread_attr_setstack(pthread_attr_t *a, void *stackaddr, size_t size)
{
	if (size < PTHREAD_STACK_MIN)
		return EINVAL;
	a->_a_stackaddr = (size_t) stackaddr + size;   /* musl stores the HIGH end internally */
	a->_a_stacksize = size;
	return 0;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **stackaddr, size_t *size)
{
	if (!a->_a_stackaddr)
		return EINVAL;                             /* no explicit stack set */
	*size = a->_a_stacksize;
	*stackaddr = (void *) (a->_a_stackaddr - a->_a_stacksize);  /* back to the LOW end */
	return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *a, size_t size)
{
	if (size > SIZE_MAX/8)
		return EINVAL;
	a->_a_guardsize = size;
	return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size)
{
	*size = a->_a_guardsize;
	return 0;
}
