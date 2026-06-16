/*
 * pthrstress — the Task 6.2 pthread STRESS gate.
 *
 * pthrtest proves each primitive works once; pthrstress proves the implementation holds up
 * under heavy, sustained load with many threads. It is DETERMINISTIC (fixed iteration counts,
 * never wall-clock waits) so it cannot flake headlessly: if any of the three sub-tests loses an
 * update, fails a create/join, or deadlocks, the single success line at the end never prints.
 *
 * Three stressors, all sized to hammer the kernel's allocThread/freeThread + kernel-stack
 * alloc/free paths (Task 6.3 headroom) and the futex-backed sync primitives:
 *
 *   1. CONTENTION — NTHREADS workers each bump a shared counter M times under one mutex. With a
 *      correct lock the total is exactly NTHREADS*M; a lost update (a torn read-modify-write)
 *      shows up as a smaller total.
 *   2. PRODUCER/CONSUMER — one producer pushes ITEMS values through a 1-slot bounded buffer
 *      guarded by a mutex + two condvars; the main thread consumes them all and checks the sum.
 *      The size-1 buffer forces a full wait/signal round-trip per item.
 *   3. CREATE/JOIN CHURN — ROUNDS rounds, each spawning NTHREADS short-lived threads and joining
 *      them all. This recycles the thread/task tables and kernel stacks ROUNDS*NTHREADS times;
 *      every create and join must succeed.
 *
 * Success line (the gate proof):
 *     pthrstress: 32 threads, counter=640000, churn 100 rounds ok
 */
#include <pthread.h>
#include <stdio.h>

#define NTHREADS 32          /* MAXTASKS = ProcTable::MAX + 8 = 1032, so 32 live threads is ample */
#define M        20000       /* per-thread counter increments => counter == NTHREADS*M == 640000 */
#define ITEMS    2000        /* producer/consumer items through the 1-slot buffer */
#define ROUNDS   100         /* create/join churn rounds => ROUNDS*NTHREADS create+join pairs */

/* ---- 1. contention: one mutex, many bumpers --------------------------------------------- */
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *bumper(void *a)
{
	(void)a;
	for (int i = 0; i < M; i++) {
		pthread_mutex_lock(&counter_lock);
		counter++;
		pthread_mutex_unlock(&counter_lock);
	}
	return 0;
}

/* ---- 2. producer/consumer over a 1-slot bounded buffer ---------------------------------- */
static pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  not_full  = PTHREAD_COND_INITIALIZER;
static int buf_val;
static int buf_full;

static void *producer(void *a)
{
	(void)a;
	for (int i = 0; i < ITEMS; i++) {
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

/* ---- 3. create/join churn: a short-lived worker that does a touch of real work ----------- */
static void *churn_worker(void *a)
{
	/* a little arithmetic so the thread genuinely runs (not optimised to a no-op) */
	long n = (long)a;
	for (volatile int i = 0; i < 200; i++)
		n += i;
	return (void *)n;
}

int main(void)
{
	/* ---- 1. contention ------------------------------------------------------------------ */
	pthread_t mt[NTHREADS];
	int bad = 0;
	for (int i = 0; i < NTHREADS; i++)
		if (pthread_create(&mt[i], 0, bumper, 0) != 0)
			bad++;
	for (int i = 0; i < NTHREADS; i++)
		if (pthread_join(mt[i], 0) != 0)
			bad++;
	if (bad || counter != (long)NTHREADS * M) {
		printf("pthrstress: contention FAIL counter=%ld want=%ld bad=%d\n",
		       counter, (long)NTHREADS * M, bad);
		return 1;
	}

	/* ---- 2. producer/consumer ----------------------------------------------------------- */
	pthread_t pt;
	long sum = 0, want = (long)ITEMS * (ITEMS - 1) / 2;   /* sum of 0..ITEMS-1 */
	int received = 0;
	if (pthread_create(&pt, 0, producer, 0) != 0) {
		printf("pthrstress: producer create FAIL\n");
		return 1;
	}
	while (received < ITEMS) {
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
	if (received != ITEMS || sum != want) {
		printf("pthrstress: prod/cons FAIL received=%d sum=%ld want=%ld\n",
		       received, sum, want);
		return 1;
	}

	/* ---- 3. create/join churn ----------------------------------------------------------- */
	int rounds = 0;
	for (int r = 0; r < ROUNDS; r++) {
		pthread_t ct[NTHREADS];
		int rbad = 0;
		for (int i = 0; i < NTHREADS; i++)
			if (pthread_create(&ct[i], 0, churn_worker, (void *)(long)i) != 0)
				rbad++;
		for (int i = 0; i < NTHREADS; i++) {
			void *cr = 0;
			/* churn_worker(i) returns i + sum(0..199) == i + 19900; validate it so the join's
			 * return value is actually checked rather than read and discarded. */
			if (pthread_join(ct[i], &cr) != 0 || (long)cr != (long)i + 19900)
				rbad++;
		}
		if (rbad) {
			printf("pthrstress: churn FAIL round=%d bad=%d\n", r, rbad);
			return 1;
		}
		rounds++;
	}

	printf("pthrstress: %d threads, counter=%ld, churn %d rounds ok\n",
	       NTHREADS, counter, rounds);
	fflush(stdout);
	return 0;
}
