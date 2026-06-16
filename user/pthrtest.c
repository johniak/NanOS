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
#include <semaphore.h>
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

/* ---- rwlock: many readers + one writer, detect torn state ------------------------------ */
#define RW_READERS 4
#define RW_ITERS   20000
static pthread_rwlock_t rwl = PTHREAD_RWLOCK_INITIALIZER;
static volatile long rw_a, rw_b;     /* writer keeps these equal; a reader must never see a!=b */
static volatile int  rw_done;
static volatile long rw_torn;        /* count of torn reads seen by any reader */

static void *rw_writer(void *p)
{
	(void)p;
	for (long i = 1; i <= RW_ITERS; i++) {
		pthread_rwlock_wrlock(&rwl);
		rw_a = i;
		rw_b = i;                    /* both updated atomically w.r.t. readers */
		pthread_rwlock_unlock(&rwl);
	}
	rw_done = 1;
	return 0;
}

static void *rw_reader(void *p)
{
	(void)p;
	while (!rw_done) {
		pthread_rwlock_rdlock(&rwl);
		if (rw_a != rw_b) rw_torn++;
		pthread_rwlock_unlock(&rwl);
	}
	return 0;
}

/* ---- barrier: N threads rendezvous, then all must see all-arrived ----------------------- */
#define BAR_THREADS 5
static pthread_barrier_t barrier;
static pthread_mutex_t bar_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int bar_arrived;
static volatile int bar_all_ok;     /* incremented by each thread that saw arrived==N post-barrier */

static void *bar_worker(void *p)
{
	(void)p;
	pthread_mutex_lock(&bar_lock);
	bar_arrived++;
	pthread_mutex_unlock(&bar_lock);
	pthread_barrier_wait(&barrier);
	/* Past the barrier, every thread must have already incremented bar_arrived. */
	if (bar_arrived == BAR_THREADS) {
		pthread_mutex_lock(&bar_lock);
		bar_all_ok++;
		pthread_mutex_unlock(&bar_lock);
	}
	return 0;
}

/* ---- once: init must run exactly once across many threads ------------------------------- */
#define ONCE_THREADS 6
static pthread_once_t once_ctl = PTHREAD_ONCE_INIT;
static volatile int once_count;

static void once_init(void) { once_count++; }
static void *once_worker(void *p)
{
	(void)p;
	pthread_once(&once_ctl, once_init);
	return 0;
}

/* ---- TLS key: each thread sees only its own value -------------------------------------- */
#define TSD_THREADS 5
static pthread_key_t tsd_key;

static void *tsd_worker(void *p)
{
	long id = (long)p;
	pthread_setspecific(tsd_key, (void *)(id + 100));
	/* spin a little so other threads interleave their setspecific */
	for (volatile int i = 0; i < 100000; i++) ;
	long got = (long)pthread_getspecific(tsd_key);
	return (void *)(got == id + 100 ? 0L : 1L);   /* 0 = saw own value */
}

/* ---- semaphore: producer posts, consumer waits ----------------------------------------- */
#define SEM_ITEMS 1000
static sem_t sem_items;
static volatile long sem_sum;

static void *sem_producer(void *p)
{
	(void)p;
	for (int i = 0; i < SEM_ITEMS; i++)
		sem_post(&sem_items);
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

	/* rwlock: 4 readers + 1 writer; a reader must never observe rw_a != rw_b. */
	pthread_t rwt[RW_READERS + 1];
	pthread_create(&rwt[0], 0, rw_writer, 0);
	for (int i = 0; i < RW_READERS; i++)
		pthread_create(&rwt[i + 1], 0, rw_reader, 0);
	for (int i = 0; i < RW_READERS + 1; i++)
		pthread_join(rwt[i], 0);
	if (rw_torn == 0)
		printf("pthrtest: rwlock ok\n");
	else
		printf("pthrtest: rwlock FAIL torn=%ld\n", rw_torn);

	/* barrier: BAR_THREADS rendezvous; every thread must see all arrived afterwards. */
	pthread_barrier_init(&barrier, 0, BAR_THREADS);
	pthread_t bt[BAR_THREADS];
	for (int i = 0; i < BAR_THREADS; i++)
		pthread_create(&bt[i], 0, bar_worker, 0);
	for (int i = 0; i < BAR_THREADS; i++)
		pthread_join(bt[i], 0);
	pthread_barrier_destroy(&barrier);
	if (bar_all_ok == BAR_THREADS)
		printf("pthrtest: barrier ok\n");
	else
		printf("pthrtest: barrier FAIL all_ok=%d\n", bar_all_ok);

	/* once: ONCE_THREADS race to run once_init; it must run exactly once. */
	pthread_t ot[ONCE_THREADS];
	for (int i = 0; i < ONCE_THREADS; i++)
		pthread_create(&ot[i], 0, once_worker, 0);
	for (int i = 0; i < ONCE_THREADS; i++)
		pthread_join(ot[i], 0);
	if (once_count == 1)
		printf("pthrtest: once ok\n");
	else
		printf("pthrtest: once FAIL count=%d\n", once_count);

	/* TLS key: each thread sets+gets its own value; main keeps its own independently. */
	pthread_key_create(&tsd_key, 0);
	pthread_setspecific(tsd_key, (void *)999L);
	pthread_t kt[TSD_THREADS];
	for (long i = 0; i < TSD_THREADS; i++)
		pthread_create(&kt[i], 0, tsd_worker, (void *)i);
	long tsd_bad = 0;
	for (int i = 0; i < TSD_THREADS; i++) {
		void *kr;
		pthread_join(kt[i], &kr);
		tsd_bad += (long)kr;
	}
	long main_tsd = (long)pthread_getspecific(tsd_key);   /* must be unchanged by threads */
	if (tsd_bad == 0 && main_tsd == 999)
		printf("pthrtest: tlskey ok\n");
	else
		printf("pthrtest: tlskey FAIL bad=%ld main=%ld\n", tsd_bad, main_tsd);
	pthread_key_delete(tsd_key);

	/* semaphore: producer posts SEM_ITEMS, main consumes them, summing the order index. */
	sem_init(&sem_items, 0, 0);
	pthread_t spt;
	pthread_create(&spt, 0, sem_producer, 0);
	for (int i = 0; i < SEM_ITEMS; i++) {
		sem_wait(&sem_items);
		sem_sum += i;
	}
	pthread_join(spt, 0);
	int sem_left = -1;
	sem_getvalue(&sem_items, &sem_left);
	sem_destroy(&sem_items);
	/* sum of 0..999 = 499500, and the semaphore must be fully drained. */
	if (sem_sum == 499500 && sem_left == 0)
		printf("pthrtest: sem ok\n");
	else
		printf("pthrtest: sem FAIL sum=%ld left=%d\n", sem_sum, sem_left);

	return 0;
}
