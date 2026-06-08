/*
 * timetest — exercise the monotonic clock (clock_gettime) and nanosleep in QEMU.
 *
 * Reads the clock, sleeps ~500 ms, reads it again, and prints the measured delta in
 * milliseconds. A delta near 500 proves both syscalls work end-to-end (Stage 1 of the
 * Doom port: doomgeneric's DG_GetTicksMs/DG_SleepMs sit on exactly these).
 */
#include <time.h>
#include <stdio.h>

static long ms_now(void) {
	struct timespec ts = { 0, 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
	long t0 = ms_now();
	printf("clock_gettime start: %ld ms\n", t0);

	struct timespec req = { 0, 500000000 };   /* 500 ms */
	nanosleep(&req, 0);

	long t1 = ms_now();
	printf("clock_gettime stop:  %ld ms\n", t1);
	printf("slept ~%ld ms (expected ~500)\n", t1 - t0);
	return 0;
}
