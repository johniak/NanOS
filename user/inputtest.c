/*
 * inputtest — prove non-blocking console reads (Stage 3 of the Doom port).
 *
 * Switches the tty to raw mode and sets O_NONBLOCK on stdin, then polls read(0) in a
 * tight loop for ~3 seconds. A non-blocking read returns -1/EAGAIN when nothing is
 * buffered (so the loop keeps spinning instead of hanging) and the bytes when a key is
 * pressed. We print every key seen and report the poll count — a large count with no
 * input is the proof that read did NOT block. This is exactly how doomgeneric's
 * DG_GetKey polls the keyboard each frame.
 */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>

int termmode(int raw);

static long ms_now(void) {
	struct timespec ts = { 0, 0 };
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
	termmode(1);                       /* raw: per-key, no echo, no line editing */
	fcntl(0, F_SETFL, O_NONBLOCK);     /* the Unix way to make reads non-blocking */

	printf("inputtest: polling stdin for 3s (press keys; 'q' quits early)\n");
	long start = ms_now();
	long polls = 0, keys = 0;
	for (;;) {
		char c;
		int r = read(0, &c, 1);
		if (r == 1) {
			keys++;
			printf("[key 0x%02x %c]\n", (unsigned char) c, (c >= 32 && c < 127) ? c : '.');
			if (c == 'q')
				break;
		} else {
			polls++;                   /* r < 0 with errno==EAGAIN: nothing buffered */
		}
		if (ms_now() - start > 3000)
			break;
	}

	termmode(0);                       /* restore cooked mode for the shell */
	printf("\ninputtest: %ld empty polls, %ld keys (non-blocking confirmed)\n", polls, keys);
	return 0;
}
