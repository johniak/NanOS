/*
 * smptorture — the SMP DATA-RACE gate for retiring the Big Kernel Lock (Phase 4, Task 15).
 *
 * pthrstress hammers the futex-backed sync primitives + thread create/join. smptorture targets the
 * structures that the BKL used to serialize and that fine-grained locks (15b-15e) now protect:
 * the per-process FD TABLE, a shared PIPE ring, and SIGNAL delivery — driven by many threads at
 * once so that on >1 core they genuinely collide. It is DETERMINISTIC (fixed counts, no wall-clock
 * waits) with order-independent oracles, so it cannot flake headlessly: any lost/duplicated byte,
 * cross-thread fd corruption, or torn counter makes the final success line never print.
 *
 *   1. FD TABLE   — main reads a stable read-only system file into a reference buffer ONCE; then NT
 *                   threads each loop: open that same file, read it, byte-compare to the reference,
 *                   close. Concurrent open/read/close churn the shared allocFd()/fds[] table; a race
 *                   (two opens collide on a slot, or a close clobbers another thread's entry) makes
 *                   a thread read through the wrong fd -> content mismatch -> fd_fail. No writes, so
 *                   it is independent of filesystem write semantics and passes cleanly when serial.
 *   2. PIPE RING  — NT writer threads each push PIPE_ITEMS copies of their own id byte through ONE
 *                   shared pipe; a consumer drains exactly NT*PIPE_ITEMS bytes and tallies per id.
 *                   Order is nondeterministic but the per-id COUNT is invariant: a ring race that
 *                   loses or duplicates a byte breaks counts[id] == PIPE_ITEMS.
 *   3. SIGNALS    — NT threads raise SIGUSR1 at the process repeatedly while a handler flips a flag;
 *                   the gate is "handler ran AND the process never crashed" (standard signals
 *                   coalesce, so the count is not a deterministic oracle — survival + delivery is).
 *
 * Success line (the gate proof):
 *     smptorture: 8 threads, fd ok, pipe ok (32000 bytes), counter=800000, signals ok
 */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>

#define NT         8        /* worker threads: oversubscribes 4 vCPUs so they genuinely collide */
#define FD_ITERS   500      /* open/write/read/close round-trips per thread */
#define PIPE_ITEMS 4000     /* id-bytes each writer pushes through the shared pipe */
#define M          4000     /* per-thread mutex-guarded counter bumps => NT*M total. Kept modest: at
                              * heavy contention each miss is a futex syscall + switch, brutally slow
                              * under single-threaded TCG; pthrstress already does the deep futex run. */

/* ---- 1. FD-table torture (read-only churn against a fixed reference) -------------------- */
#define FD_REFPATH "/disks/main/nanos/bin/true.nxe"   /* a small, stable, world-readable system file */
#define FD_REFLEN  256
static volatile int fd_fail = 0;
static unsigned char fd_ref[FD_REFLEN];
static int fd_reflen = 0;

static void *fd_worker(void *a)
{
	(void)a;
	for (int i = 0; i < FD_ITERS; i++) {
		int fd = open(FD_REFPATH, O_RDONLY);
		if (fd < 0) { fd_fail = 1; return 0; }
		unsigned char rd[FD_REFLEN];
		int r = (int)read(fd, rd, fd_reflen);
		close(fd);
		/* a correct, serialized fd table hands this thread its own fd onto FD_REFPATH, so the
		 * read MUST reproduce the reference exactly; a slot collision / cross-close yields a
		 * short or wrong read. */
		if (r != fd_reflen || memcmp(rd, fd_ref, fd_reflen) != 0) { fd_fail = 1; return 0; }
	}
	return 0;
}

/* ---- 2. shared-pipe torture ------------------------------------------------------------- */
static int pipe_fd[2];

static void *pipe_writer(void *a)
{
	long id = (long)a;
	unsigned char b = (unsigned char)id;
	unsigned char chunk[256];
	for (int k = 0; k < 256; k++) chunk[k] = b;
	int left = PIPE_ITEMS;
	while (left > 0) {
		int want = left < 256 ? left : 256;
		int w = (int)write(pipe_fd[1], chunk, want);   /* may be partial when the ring is full */
		if (w <= 0) return (void *)1;                  /* a real error -> non-null return */
		left -= w;
	}
	return 0;
}

/* ---- 3. signal torture ------------------------------------------------------------------ */
static volatile int sig_seen = 0;
static void on_usr1(int s) { (void)s; sig_seen = 1; }

static void *sig_worker(void *a)
{
	(void)a;
	for (int i = 0; i < 200; i++)
		kill(getpid(), SIGUSR1);
	return 0;
}

/* ---- 4. futex contention (lost-update oracle) ------------------------------------------- */
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

int main(void)
{
	pthread_t t[NT];
	printf("smptorture: start\n"); fflush(stdout);

	/* 1. FD table — load the reference once (serial), then churn it from NT threads */
	{
		int fd = open(FD_REFPATH, O_RDONLY);
		if (fd < 0) { printf("smptorture: fd ref open FAIL (%s)\n", FD_REFPATH); return 1; }
		fd_reflen = (int)read(fd, fd_ref, FD_REFLEN);
		close(fd);
		if (fd_reflen <= 0) { printf("smptorture: fd ref read FAIL (%d)\n", fd_reflen); return 1; }
	}
	for (long i = 0; i < NT; i++)
		if (pthread_create(&t[i], 0, fd_worker, (void *)i) != 0) { printf("smptorture: fd create FAIL\n"); return 1; }
	for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
	if (fd_fail) { printf("smptorture: FD-TABLE RACE — read-back mismatch\n"); return 1; }
	printf("smptorture: fd phase done\n"); fflush(stdout);

	/* 2. shared pipe: NT writers, this thread the consumer */
	if (pipe(pipe_fd) != 0) { printf("smptorture: pipe() FAIL\n"); return 1; }
	for (long i = 0; i < NT; i++)
		if (pthread_create(&t[i], 0, pipe_writer, (void *)i) != 0) { printf("smptorture: pipe create FAIL\n"); return 1; }
	long counts[NT];
	for (int i = 0; i < NT; i++) counts[i] = 0;
	long total = (long)NT * PIPE_ITEMS, got = 0;
	unsigned char rb[512];
	while (got < total) {
		int r = (int)read(pipe_fd[0], rb, sizeof rb);
		if (r <= 0) { printf("smptorture: PIPE read returned %d at %ld/%ld\n", r, got, total); return 1; }
		for (int k = 0; k < r; k++) {
			unsigned id = rb[k];
			if (id >= (unsigned)NT) { printf("smptorture: PIPE RACE — stray byte %u\n", id); return 1; }
			counts[id]++;
		}
		got += r;
	}
	for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
	close(pipe_fd[0]); close(pipe_fd[1]);
	for (int i = 0; i < NT; i++)
		if (counts[i] != PIPE_ITEMS) {
			printf("smptorture: PIPE RACE — id %d count=%ld want=%d\n", i, counts[i], PIPE_ITEMS);
			return 1;
		}
	printf("smptorture: pipe phase done\n"); fflush(stdout);

	/* 3. signals */
	signal(SIGUSR1, on_usr1);
	for (long i = 0; i < NT; i++)
		if (pthread_create(&t[i], 0, sig_worker, (void *)i) != 0) { printf("smptorture: sig create FAIL\n"); return 1; }
	for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
	if (!sig_seen) { printf("smptorture: SIGNAL FAIL — handler never ran\n"); return 1; }
	printf("smptorture: signal phase done\n"); fflush(stdout);

	/* 4. futex contention */
	counter = 0;
	for (long i = 0; i < NT; i++)
		if (pthread_create(&t[i], 0, bumper, (void *)i) != 0) { printf("smptorture: bump create FAIL\n"); return 1; }
	for (int i = 0; i < NT; i++) pthread_join(t[i], 0);
	if (counter != (long)NT * M) {
		printf("smptorture: FUTEX RACE — counter=%ld want=%ld\n", counter, (long)NT * M);
		return 1;
	}

	printf("smptorture: %d threads, fd ok, pipe ok (%ld bytes), counter=%ld, signals ok\n",
	       NT, total, counter);
	fflush(stdout);
	return 0;
}
