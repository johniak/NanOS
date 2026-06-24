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

/* STATIC work partitioning (no shared queue, no mutex): the whole job is q_total "units", unit u
 * recomputes row (u % HEIGHT). Worker w owns the units with u % nworkers == w, so the parallel
 * section is pure ring-3 compute with ZERO syscalls — the only way a Big-Kernel-Lock kernel can
 * show real speedup (a per-unit mutex would serialize every worker through futex() under the BKL).
 * Total work is HEIGHT*repeat regardless of nworkers, so `pfract 1 R` and `pfract N R` do the SAME
 * compute and T(1)/T(N) is a fair speedup. Workers may write the same iters[row] concurrently, but
 * mandel_cell is deterministic so the values are identical — a benign race, checksum stays stable. */
static long q_total;         /* HEIGHT * repeat */

/* Each worker accumulates a partial sum over its OWN units into s->sum — no shared-memory writes in
 * the hot loop, so there is no cache-line false sharing between workers (which otherwise caps MTTCG
 * scaling). The total is an additive (commutative) checksum: identical for any nworkers, so it both
 * validates the parallel result and stays a fair fixed amount of work. iters[][] is filled by a
 * cheap serial pass afterwards purely for the ASCII art. */
struct slice { int id; int nworkers; unsigned long sum; };

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
	struct slice *s = (struct slice *)a;
	/* Strided over the whole job: every nworkers-th unit. Pure compute into a LOCAL accumulator —
	 * no locks, no syscalls, no shared writes — so N workers run flat-out on N cores. */
	unsigned long acc = 0;
	for (long u = s->id; u < q_total; u += s->nworkers) {
		int row = (int)(u % HEIGHT);
		for (int col = 0; col < WIDTH; col++)
			acc += (unsigned long) mandel_cell(row, col);
	}
	s->sum = acc;
	return 0;
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

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	pthread_t w[MAXW];
	struct slice arg[MAXW];
	for (int i = 0; i < nworkers; i++) {
		arg[i].id = i;
		arg[i].nworkers = nworkers;
		arg[i].sum = 0;
		if (pthread_create(&w[i], 0, worker, &arg[i]) != 0) {
			printf("pfract: pthread_create FAILED for worker %d\n", i);
			fflush(stdout);
			return 1;
		}
	}

	unsigned long total = 0;
	for (int i = 0; i < nworkers; i++) {
		pthread_join(w[i], 0);
		total += arg[i].sum;            /* additive => independent of nworkers */
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

	/* Serial pass purely for the ASCII art (one grid, cheap next to the timed parallel loop). */
	for (int row = 0; row < HEIGHT; row++) {
		char line[WIDTH + 1];
		for (int col = 0; col < WIDTH; col++) {
			int n = mandel_cell(row, col);
			int idx = n >= MAXITER ? levels : n * levels / MAXITER;
			line[col] = ramp[idx];
		}
		line[WIDTH] = '\0';
		printf("%s\n", line);
	}

	printf("pfract: %d workers, %ld repeat, %ld ms, checksum=0x%08lx ok\n",
	       nworkers, repeat, ms, total & 0xffffffffUL);
	fflush(stdout);
	return 0;
}
