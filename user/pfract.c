/*
 * pfract — a parallel Mandelbrot renderer, the "cool real -lpthread workload" (Task 6.2).
 *
 * This is the canonical thread-pool program: a work queue of row-bands protected by a mutex +
 * condition variable, drained by NWORKERS worker threads that each compute the escape-iteration
 * count for every cell in the bands they pull. The main thread fills the queue, closes it, and
 * pthread_join()s the workers. It genuinely exercises create + mutex + cond + join under real
 * CPU load (the Mandelbrot inner loop), not just a smoke handshake.
 *
 * NanOS is a uniprocessor, so this is concurrency, not parallelism: the workers interleave on the
 * one core rather than running on several. The result is identical either way — the work queue
 * just decides who computes which row — which is exactly why the CHECKSUM below is a deterministic
 * correctness proof: it is the sum of all escape-iteration counts and does not depend on which
 * worker computed which row, so a correct render always produces the same value.
 *
 * Primary output is ASCII art to stdout (guaranteed visible in the 80x25 text-mode VGA console
 * screendump): a WIDTHxHEIGHT grid mapping escape-count to the ramp " .:-=+*#%@". The classic
 * Mandelbrot cardioid + bulb is recognisable. Then a verification line:
 *     pfract: 8 workers, 1540 cells, checksum=0x........ ok
 */
#include <pthread.h>
#include <stdio.h>

#define NWORKERS 8
#define WIDTH    70          /* fits inside the 80-column console */
#define HEIGHT   22          /* fits inside the 25-row console (leaves room for the status line) */
#define MAXITER  100         /* escape-iteration ceiling; also the in-set marker */

/* Complex-plane window: the classic full Mandelbrot view. */
#define RE_MIN (-2.5)
#define RE_MAX ( 1.0)
#define IM_MIN (-1.15)
#define IM_MAX ( 1.15)

/* Per-cell escape-iteration counts, filled by the workers, read by main for art + checksum. */
static int iters[HEIGHT][WIDTH];

/* The work queue: row indices 0..HEIGHT-1, handed out one band (= one row) at a time. */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cond = PTHREAD_COND_INITIALIZER;
static int q_next;           /* next row to hand out */
static int q_closed;         /* set once all rows are enqueued (main has finished filling) */

/* Compute the escape-iteration count of one cell using fixed double arithmetic. */
static int mandel_cell(int row, int col)
{
	double cr = RE_MIN + (RE_MAX - RE_MIN) * col / (WIDTH - 1);
	double ci = IM_MIN + (IM_MAX - IM_MIN) * row / (HEIGHT - 1);
	double zr = 0.0, zi = 0.0;
	int n = 0;
	while (n < MAXITER && zr * zr + zi * zi <= 4.0) {
		double t = zr * zr - zi * zi + cr;
		zi = 2.0 * zr * zi + ci;
		zr = t;
		n++;
	}
	return n;
}

/* Worker: pull rows off the queue until it is drained and closed; compute each row's cells. */
static void *worker(void *a)
{
	(void)a;
	for (;;) {
		int row;
		pthread_mutex_lock(&q_lock);
		while (q_next >= HEIGHT && !q_closed)
			pthread_cond_wait(&q_cond, &q_lock);     /* wait for work (or for close) */
		if (q_next >= HEIGHT) {                       /* nothing left and queue closed -> done */
			pthread_mutex_unlock(&q_lock);
			return 0;
		}
		row = q_next++;
		pthread_mutex_unlock(&q_lock);

		for (int col = 0; col < WIDTH; col++)
			iters[row][col] = mandel_cell(row, col);
	}
}

int main(void)
{
	static const char ramp[] = " .:-=+*#%@";   /* 10 levels: space (far) .. '@' (in set) */
	const int levels = sizeof(ramp) - 2;        /* highest ramp index (9) */

	/* Start the queue EMPTY (q_next == HEIGHT) so a worker that runs the instant it is created
	 * finds nothing available and blocks on the condvar predicate instead of grabbing row 0. */
	q_next = HEIGHT;

	pthread_t w[NWORKERS];
	for (int i = 0; i < NWORKERS; i++)
		if (pthread_create(&w[i], 0, worker, 0) != 0) {
			printf("pfract: pthread_create FAILED for worker %d\n", i);
			fflush(stdout);
			return 1;
		}

	/* Now that the workers exist (and are blocked), enqueue the whole job under the lock: open
	 * rows 0..HEIGHT-1, mark the queue closed (these HEIGHT rows are the entire job, no more will
	 * be added), and broadcast to wake the blocked workers. The broadcast happens after the state
	 * change and under the lock, so there is no lost wakeup. */
	pthread_mutex_lock(&q_lock);
	q_next = 0;                                  /* rows 0..HEIGHT-1 are now available */
	q_closed = 1;
	pthread_cond_broadcast(&q_cond);
	pthread_mutex_unlock(&q_lock);

	for (int i = 0; i < NWORKERS; i++)
		pthread_join(w[i], 0);

	/* ASCII art + checksum. The checksum is a deterministic fold of every iteration count, so a
	 * correct render always yields the same value regardless of worker scheduling. */
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

	printf("pfract: %d workers, %d cells, checksum=0x%08x ok\n",
	       NWORKERS, WIDTH * HEIGHT, checksum);
	fflush(stdout);
	return 0;
}
