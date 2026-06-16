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
 * Phase 5 (Task 5.3) adds pthread_cancel: a thread blocked in a cancellation point is cancelled
 * and joined with PTHREAD_CANCELED, and its cleanup handler runs.
 *
 * Expect, in order:
 *     pthrtest: join r=42
 *     pthrtest: counter=40000
 *     pthrtest: condvar ok sum=499500
 *     ... (rwlock/barrier/once/tlskey/sem) ...
 *     pthrtest: cancel ok
 *     pthrtest: cancel reuse ok
 */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>     /* strcmp (fork/exec-mt sentinel) */

/* fork/exec are not in the slim glue headers — declared here as nsh/forkmany do. */
int  fork(void);
int  execve(const char *path, char *const argv[], char *const envp[]);
int  waitpid(int pid, int *status, int options);
void _exit(int code);
extern char **environ;

#ifndef WIFEXITED
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#endif

static void *worker(void *a) { return (void *)((long)a + 1); }

/* ---- fork/exec in a multithreaded process (Task 6.1) ----------------------------------- */
#define FMT_WORKERS 3
static volatile int fmt_stop;     /* sibling workers spin until the main thread sets this */

/* A pure spinner: no malloc/stdio, so the process is genuinely multithreaded yet a fork from it
 * cannot deadlock on a libc lock held by a sibling (bash forks+execs the same way). */
static void *fmt_spinner(void *a)
{
	(void)a;
	while (!fmt_stop)
		for (volatile int i = 0; i < 2000; i++) ;
	return 0;
}

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

/* ---- cancellation: a thread blocked in a cancellation point is cancelled ----------------
 * The cancellation point here is pthread_cond_wait: it routes through __timedwait_cp ->
 * (__syscall_cp), which honours a pending cancel both at entry and on a SIGCANCEL-driven
 * -EINTR while parked in the futex. IMPORTANT: picolibc's blocking calls (sleep/read/...) are
 * NOT routed through __syscall_cp in this hybrid libc, so they are NOT cancellation points —
 * a futex-based wait (cond/sem) is required to be cancellable. The cleanup handler pushed
 * before the wait must run on cancellation (POSIX), exiting the thread with PTHREAD_CANCELED. */
static pthread_mutex_t cxl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cxl_cond = PTHREAD_COND_INITIALIZER;
static volatile int    cxl_ready;
static volatile int    cxl_cleanup_ran;

/* POSIX idiom: pthread_cond_wait re-acquires the mutex before cancellation runs cleanup
 * handlers, so the handler is responsible for releasing it. Unlocking here is also what lets
 * the condvar-reuse check below relock cxl_lock after the cancelled worker is gone. */
static void cxl_cleanup(void *p) { (void)p; cxl_cleanup_ran = 1; pthread_mutex_unlock(&cxl_lock); }

static void *cxl_worker(void *p)
{
	(void)p;
	pthread_mutex_lock(&cxl_lock);
	pthread_cleanup_push(cxl_cleanup, 0);
	cxl_ready = 1;
	/* Never-signalled condvar: block until cancelled (loop guards against any spurious wake). */
	while (1)
		pthread_cond_wait(&cxl_cond, &cxl_lock);
	pthread_cleanup_pop(0);                 /* never reached: cancellation exits inside the wait */
	pthread_mutex_unlock(&cxl_lock);
	return 0;
}

/* ---- cancellation regression: REUSE the same condvar after a cancel -------------------- *
 * The cancelled cxl_worker had a stack `struct waiter node` linked into cxl_cond's waiter list
 * while parked. The fix unlinks that node as the wait unwinds (-ECANCELED). If it leaked (the
 * old bug, where a masked cancel exited the thread immediately mid-wait), a later signal on the
 * SAME condvar would walk the dead node, "deliver" the wakeup to the corpse, and never wake a
 * live waiter behind it — a lost wakeup that hangs this check forever. So a fresh waiter that
 * waits on cxl_cond and then gets signalled MUST actually wake. */
static volatile int cxl2_ready;
static volatile int cxl2_woke;
static int          cxl2_pred;

static void *cxl_reuse_worker(void *p)
{
	(void)p;
	pthread_mutex_lock(&cxl_lock);
	cxl2_ready = 1;
	while (!cxl2_pred)
		pthread_cond_wait(&cxl_cond, &cxl_lock);
	cxl2_woke = 1;
	pthread_mutex_unlock(&cxl_lock);
	return 0;
}

int main(int argc, char **argv)
{
	/* Re-exec entry: a forked child that itself went multithreaded execve'd us with this arg.
	 * Reaching main here proves execve from a multithreaded process tore down the siblings and
	 * loaded the fresh (single-threaded) image cleanly. */
	if (argc > 1 && argv[1] && !strcmp(argv[1], "--fork-mt-exec-child")) {
		printf("pthrtest: exec-mt child ok\n");
		fflush(stdout);
		_exit(0);
	}

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

	/* cancellation: cancel a thread parked in pthread_cond_wait; it must join as
	 * PTHREAD_CANCELED and its cleanup handler must have run. */
	cxl_ready = 0;
	cxl_cleanup_ran = 0;
	pthread_t ct;
	pthread_create(&ct, 0, cxl_worker, 0);
	while (!cxl_ready) ;                       /* wait until it has entered the critical section */
	for (volatile long i = 0; i < 5000000L; i++) ;   /* let it actually park in the futex */
	pthread_cancel(ct);
	void *cr = 0;
	pthread_join(ct, &cr);
	if (cr == PTHREAD_CANCELED && cxl_cleanup_ran)
		printf("pthrtest: cancel ok\n");
	else
		printf("pthrtest: cancel FAIL res=%p cleanup=%d\n", cr, cxl_cleanup_ran);

	/* cancel regression: REUSE cxl_cond. A fresh waiter on the same condvar must still be
	 * wakeable; a leaked dead waiter node from the cancelled thread would lose this wakeup and
	 * hang the join. (The cancelled worker's cleanup released cxl_lock, so we can relock it.) */
	cxl2_ready = 0;
	cxl2_woke = 0;
	cxl2_pred = 0;
	pthread_t ct2;
	pthread_create(&ct2, 0, cxl_reuse_worker, 0);
	while (!cxl2_ready) ;                       /* wait until it holds the lock */
	for (volatile long i = 0; i < 5000000L; i++) ;   /* let it actually park in the futex */
	pthread_mutex_lock(&cxl_lock);
	cxl2_pred = 1;
	pthread_cond_signal(&cxl_cond);
	pthread_mutex_unlock(&cxl_lock);
	pthread_join(ct2, 0);                       /* hangs forever on a lost wakeup */
	if (cxl2_woke)
		printf("pthrtest: cancel reuse ok\n");
	else
		printf("pthrtest: cancel reuse FAIL\n");

	/* fork/exec in a multithreaded process (Task 6.1). Spin up FMT_WORKERS sibling threads so the
	 * process is genuinely multithreaded, then fork(): POSIX hands the child ONLY the calling
	 * thread. The child proves it runs correctly single-threaded, THEN spawns threads of its own
	 * and execve's — which exercises the execve sibling-teardown path (the re-exec'd image reports
	 * "exec-mt child ok"). The parent waitpid's the child, asserts exit 0, then stops + joins its
	 * workers. */
	fmt_stop = 0;
	pthread_t fw[FMT_WORKERS];
	for (int i = 0; i < FMT_WORKERS; i++)
		pthread_create(&fw[i], 0, fmt_spinner, 0);
	fflush(stdout);                         /* drain buffered output so the fork child can't dup it */
	int fmt_pid = fork();
	if (fmt_pid == 0) {
		/* CHILD: a single-threaded copy of the calling (main) thread. Do real work to prove it is
		 * alive and correct on its own (the sibling stacks copied into our space are dead memory). */
		long sum = 0;
		for (long i = 1; i <= 1000; i++) sum += i;     /* == 500500 */
		if (sum != 500500)
			_exit(3);
		printf("pthrtest: fork-mt child alive\n");
		fflush(stdout);
		/* Now make the CHILD itself multithreaded, then execve — exercising sibling teardown. */
		pthread_t cw[2];
		pthread_create(&cw[0], 0, fmt_spinner, 0);
		pthread_create(&cw[1], 0, fmt_spinner, 0);
		char *cargv[] = { (char *)"pthrtest", (char *)"--fork-mt-exec-child", 0 };
		execve("/disks/main/apps/pthrtest/pthrtest.nxe", cargv, environ);
		_exit(127);                          /* execve only returns on failure */
	}
	int fmt_st = 0;
	int fmt_w = waitpid(fmt_pid, &fmt_st, 0);
	fmt_stop = 1;                            /* release the workers */
	for (int i = 0; i < FMT_WORKERS; i++)
		pthread_join(fw[i], 0);
	if (fmt_pid > 0 && fmt_w == fmt_pid && WIFEXITED(fmt_st) && WEXITSTATUS(fmt_st) == 0)
		printf("pthrtest: fork-mt ok\n");
	else
		printf("pthrtest: fork-mt FAIL pid=%d w=%d st=0x%x\n", fmt_pid, fmt_w, fmt_st);

	return 0;
}
