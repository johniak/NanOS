/*
 * pthrtest — the Phase 4 pthread keystone smoke test, in the POSIX API.
 *
 * Task 4.2 drove pthread_create/join end to end through the libc.ndl pthread layer. Task 4.3
 * adds the synchronization primitives:
 *
 *   - a mutex stress: 4 threads each take a shared pthread_mutex_t and bump a shared counter
 *     10000 times; if the lock truly serializes the read-modify-write the total is exactly
 *     40000 (a missing/broken lock loses increments to the race).
 *   - a condvar handshake: a producer thread pushes N items into a 1-slot bounded buffer
 *     guarded by a mutex + two condition variables (not-full / not-empty); the main thread
 *     consumes them. Exercises pthread_cond_wait's FUTEX_REQUEUE wait/signal sequencing under
 *     real blocking (the buffer is size 1, so every item forces a full wait/signal round-trip).
 *
 * Expect, in order:
 *     pthrtest: join r=42
 *     pthrtest: counter=40000
 *     pthrtest: condvar ok sum=499500
 */
#include <pthread.h>
#include <stdio.h>

static void *worker(void *a) { return (void *)((long)a + 1); }

/* ---- mutex stress ---------------------------------------------------------------------- */
#define MUTEX_THREADS 4
#define MUTEX_ITERS   10000
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *bumper(void *a)
{
	(void)a;
	for (int i = 0; i < MUTEX_ITERS; i++) {
		pthread_mutex_lock(&counter_lock);
		counter++;
		pthread_mutex_unlock(&counter_lock);
	}
	return 0;
}

/* ---- producer/consumer over a condvar -------------------------------------------------- */
#define COND_ITEMS 1000
static pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t not_full  = PTHREAD_COND_INITIALIZER;
static int buf_val;
static int buf_full;

static void *producer(void *a)
{
	(void)a;
	for (int i = 0; i < COND_ITEMS; i++) {
		pthread_mutex_lock(&buf_lock);
		while (buf_full)
			pthread_cond_wait(&not_full, &buf_lock);
		buf_val = i;
		buf_full = 1;
		pthread_cond_signal(&not_empty);
		pthread_mutex_unlock(&buf_lock);
	}
	return 0;
}

int main(void)
{
	pthread_t t;
	void *r;

	pthread_create(&t, 0, worker, (void *)41);
	pthread_join(t, &r);
	printf("pthrtest: join r=%ld\n", (long)r);   /* expect 42 */

	/* mutex stress: 4 threads, 10000 increments each => 40000 */
	pthread_t mt[MUTEX_THREADS];
	for (int i = 0; i < MUTEX_THREADS; i++)
		pthread_create(&mt[i], 0, bumper, 0);
	for (int i = 0; i < MUTEX_THREADS; i++)
		pthread_join(mt[i], 0);
	printf("pthrtest: counter=%ld\n", counter);   /* expect 40000 */

	/* producer/consumer: consume COND_ITEMS items, summing them. */
	pthread_t pt;
	long sum = 0;
	int received = 0;
	pthread_create(&pt, 0, producer, 0);
	while (received < COND_ITEMS) {
		pthread_mutex_lock(&buf_lock);
		while (!buf_full)
			pthread_cond_wait(&not_empty, &buf_lock);
		sum += buf_val;
		buf_full = 0;
		received++;
		pthread_cond_signal(&not_full);
		pthread_mutex_unlock(&buf_lock);
	}
	pthread_join(pt, 0);
	/* sum of 0..999 = 999*1000/2 = 499500 */
	if (received == COND_ITEMS && sum == 499500)
		printf("pthrtest: condvar ok sum=%ld\n", sum);
	else
		printf("pthrtest: condvar FAIL received=%d sum=%ld\n", received, sum);

	return 0;
}
