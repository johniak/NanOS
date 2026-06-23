/*
 * pfract — a parallel Mandelbrot renderer + the SMP parallel-speedup gate (Task 6.2 / SMP Task 10).
 *
 * A work queue of row-bands protected by a mutex + condition variable, drained by N worker threads
 * that each compute the escape-iteration count for the bands they pull. The main thread fills the
 * queue, closes it, joins the workers, then prints ASCII art + a deterministic checksum. It
 * genuinely exercises pthread create + mutex + cond + join under real CPU load.
 *
 * INTEGER fixed-point (scale 2^16): NanOS does NOT save FPU/XMM across a context switch (the kernel
 * is -mno-sse and the IRQ stub saves only GPRs), so a double-precision worker preempted mid-cell
 * could see corrupted XMM. Integer state lives in GPRs, which the trap frame preserves — so this
 * stays correct whether the workers interleave on one core or run truly in parallel on several.
 * That is what makes the checksum a valid multicore correctness proof.
 *
 * Usage: pfract [workers] [repeat]
 *   workers — worker thread count (default 8, clamped 1..16).
 *   repeat  — recompute the whole grid this many times (default 1) for a heavier, timeable load.
 * Prints, after the art:  pfract: N workers, R repeat, T ms, checksum=0x........ ok
 * The SMP gate runs `pfract 1 R` vs `pfract 4 R` on a 4-vCPU boot and checks T(1)/T(4) is a real
 * speedup — work in the compute loop is pure ring-3 (no syscalls), so it runs off the Big Kernel
 * Lock and scales with cores.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAXW     16
#define WIDTH    70          /* fits inside the 80-column console */
#define HEIGHT   22          /* fits inside the 25-row console (leaves room for the status line) */
#define MAXITER  100         /* escape-iteration ceiling; also the in-set marker */

/* Fixed-point complex-plane window (scale S = 1<<16). The classic full Mandelbrot view. */
#define FP 16
#define S  (1 << FP)
#define RE_MIN  (-164659L)   /* -2.513 * S */
#define RE_SPAN ( 229376L)   /*  3.5   * S */
#define IM_MIN  ( -75366L)   /* -1.15  * S */
#define IM_SPAN ( 150733L)   /*  2.3   * S */
#define ESCAPE  (4L * S)     /* |z|^2 > 4 */

/* Per-cell escape-iteration counts, filled by the workers, read by main for art + checksum. */
static int iters[HEIGHT][WIDTH];

/* The work queue: units 0..total-1; unit u computes row (u % HEIGHT). total = HEIGHT * repeat. */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cond = PTHREAD_COND_INITIALIZER;
static long q_next;          /* next unit to hand out */
static long q_total;         /* HEIGHT * repeat */
static int  q_closed;        /* set once all units are enqueued */

/* Escape-iteration count of one cell, fixed-point int64 (no floating point). */
static int mandel_cell(int row, int col)
{
	long cr = RE_MIN + RE_SPAN * col / (WIDTH - 1);
	long ci = IM_MIN + IM_SPAN * row / (HEIGHT - 1);
	long zr = 0, zi = 0;
	int n = 0;
	for (;;) {
		long zr2 = (zr * zr) >> FP;
		long zi2 = (zi * zi) >> FP;
		if (zr2 + zi2 > ESCAPE || n >= MAXITER)
			break;
		long t = zr2 - zi2 + cr;
		zi = ((2 * zr * zi) >> FP) + ci;
		zr = t;
		n++;
	}
	return n;
}

static void *worker(void *a)
{
	(void)a;
	for (;;) {
		long u;
		pthread_mutex_lock(&q_lock);
		while (q_next >= q_total && !q_closed)
			pthread_cond_wait(&q_cond, &q_lock);
		if (q_next >= q_total) {
			pthread_mutex_unlock(&q_lock);
			return 0;
		}
		u = q_next++;
		pthread_mutex_unlock(&q_lock);

		int row = (int)(u % HEIGHT);
		for (int col = 0; col < WIDTH; col++)
			iters[row][col] = mandel_cell(row, col);
	}
}

int main(int argc, char **argv)
{
	static const char ramp[] = " .:-=+*#%@";   /* 10 levels: space (far) .. '@' (in set) */
	const int levels = sizeof(ramp) - 2;

	int nworkers = argc > 1 ? atoi(argv[1]) : 8;
	long repeat  = argc > 2 ? atol(argv[2]) : 1;
	if (nworkers < 1) nworkers = 1;
	if (nworkers > MAXW) nworkers = MAXW;
	if (repeat < 1) repeat = 1;

	q_total = (long)HEIGHT * repeat;
	q_next  = q_total;   /* start EMPTY so a worker that runs immediately blocks on the predicate */

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	pthread_t w[MAXW];
	for (int i = 0; i < nworkers; i++)
		if (pthread_create(&w[i], 0, worker, 0) != 0) {
			printf("pfract: pthread_create FAILED for worker %d\n", i);
			fflush(stdout);
			return 1;
		}

	pthread_mutex_lock(&q_lock);
	q_next = 0;                                  /* units 0..q_total-1 now available */
	q_closed = 1;
	pthread_cond_broadcast(&q_cond);
	pthread_mutex_unlock(&q_lock);

	for (int i = 0; i < nworkers; i++)
		pthread_join(w[i], 0);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

	unsigned int checksum = 0;
	for (int row = 0; row < HEIGHT; row++) {
		char line[WIDTH + 1];
		for (int col = 0; col < WIDTH; col++) {
			int n = iters[row][col];
			checksum = checksum * 31u + (unsigned int)n;
			int idx = n >= MAXITER ? levels : n * levels / MAXITER;
			line[col] = ramp[idx];
		}
		line[WIDTH] = '\0';
		printf("%s\n", line);
	}

	printf("pfract: %d workers, %ld repeat, %ld ms, checksum=0x%08x ok\n",
	       nworkers, repeat, ms, checksum);
	fflush(stdout);
	return 0;
}
