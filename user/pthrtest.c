/*
 * pthrtest — the Phase 4 (Task 4.2) pthread keystone smoke test, in the POSIX API.
 *
 * Where clonetest exercised the raw clone(2)/futex(2) machinery, this drives the libc.ndl
 * pthread layer end to end: pthread_create spawns a worker, pthread_join blocks until it
 * exits (the kernel CLEARTID-wakes the join word) and harvests its return value. Expect:
 *
 *     pthrtest: join r=42
 */
#include <pthread.h>
#include <stdio.h>

static void *worker(void *a) { return (void *)((long)a + 1); }

int main(void)
{
	pthread_t t;
	void *r;
	pthread_create(&t, 0, worker, (void *)41);
	pthread_join(t, &r);
	printf("pthrtest: join r=%ld\n", (long)r);   /* expect 42 */
	return 0;
}
