/* Plan 10 Task 6 — pthread / TLS smoke for the x86_64 ring-3 userland.
 *
 * Proves the two things the arch port (Tasks 1-3) had to get right at RUNTIME:
 *   (a) thread-local storage via %fs.base — touching `errno` is the canonical probe
 *       (the original Plan-6 #2 fault was the first errno access hitting an unset TLS
 *       base and #PF'ing at a low address);
 *   (b) pthread_create / pthread_join — a real second thread (clone + a fresh TCB/%fs)
 *       runs, prints, and is joined.
 *
 * Built by `make pthread-smoke` against the injected x86_64-nanos sysroot (musl pthread
 * on picolibc, bound by name through libc.ndl).
 */
#include <stdio.h>
#include <errno.h>
#include <pthread.h>

static void *worker(void *arg)
{
	(void)arg;
	printf("thread running\n");
	fflush(stdout);
	return (void *)0;
}

int main(void)
{
	errno = 0;
	printf("errno@%p\n", (void *)&errno);
	fflush(stdout);

	pthread_t t;
	int rc = pthread_create(&t, 0, worker, 0);
	if (rc != 0) {
		printf("pthread_create failed: %d\n", rc);
		fflush(stdout);
		return 1;
	}
	pthread_join(t, 0);

	printf("joined ok\n");
	fflush(stdout);
	return 0;
}
