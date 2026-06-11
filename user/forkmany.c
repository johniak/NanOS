/*
 * forkmany — prove the process/task tables are no longer capped at a tiny constant.
 * Forks as many children as asked (default 50, well past the old 16/18 ceiling); each child
 * just sleeps briefly so they are all ALIVE at once, then exits. The parent reports how many
 * concurrent children it managed to start, then reaps them all. A clean "spawned N" with N at
 * the requested count (and no fork errors) shows the limit is now memory, not an array size.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int waitpid(int pid, int* status, int options);   /* not in the glue headers; declared as in nsh */

int main(int argc, char** argv) {
	int want = (argc > 1) ? atoi(argv[1]) : 50;
	if (want < 1) want = 1;

	int spawned = 0;
	for (int i = 0; i < want; i++) {
		int pid = fork();
		if (pid == 0) {            /* child: stay alive while the parent keeps forking, then go */
			usleep(700 * 1000);
			_exit(0);
		}
		if (pid < 0) {             /* fork failed -> we hit the real (memory) ceiling */
			printf("forkmany: fork failed after %d children (errno path)\n", spawned);
			break;
		}
		spawned++;
	}

	printf("forkmany: spawned %d concurrent children (wanted %d)\n", spawned, want);
	fflush(stdout);

	int reaped = 0, st;
	while (waitpid(-1, &st, 0) > 0)
		reaped++;
	printf("forkmany: reaped %d children, done\n", reaped);
	fflush(stdout);
	return 0;
}
