/*
 * orphan — stress the orphan-reaping path. Each round forks a child that itself forks a
 * grandchild and then exits immediately, so the grandchild outlives its parent and is
 * orphaned. The kernel must re-home it on init (pid 1), and init must reap it — otherwise
 * every grandchild leaks a process slot + an 8 KB kernel stack forever. After N rounds,
 * `ls /proc` should show only a handful of live processes, not N of them.
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int waitpid(int pid, int* status, int options);

int main(int argc, char** argv) {
	int n = (argc > 1) ? atoi(argv[1]) : 100;
	if (n < 1) n = 1;

	for (int i = 0; i < n; i++) {
		int pid = fork();
		if (pid == 0) {                 /* child */
			int g = fork();
			if (g == 0) {               /* grandchild: outlive the child, then go */
				usleep(250 * 1000);
				_exit(0);               /* exits as an orphan -> must be reaped by init */
			}
			_exit(0);                   /* child exits NOW, orphaning the grandchild */
		}
		if (pid > 0)
			waitpid(pid, 0, 0);         /* reap the child; the grandchild is now init's */
	}

	printf("orphan: created %d orphans; waiting for them to exit + be reaped\n", n);
	fflush(stdout);
	usleep(800 * 1000);                 /* let the last orphans exit and init collect them */
	printf("orphan: done -- check 'ls /proc' for leaks\n");
	fflush(stdout);
	return 0;
}
