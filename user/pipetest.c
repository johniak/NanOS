/*
 * pipetest — exercises the Stage-1 plumbing: pipe() + fork() + dup2() + poll().
 * The child redirects its stdout onto the pipe's write end (dup2) and printf()s a line;
 * the parent polls the read end, then reads the bytes back. Proves pipes carry data
 * across a fork and that poll reports readiness.
 */
#include <unistd.h>
#include <stdio.h>
#include <poll.h>

int main(void) {
	int fd[2];
	if (pipe(fd) < 0) {
		printf("pipetest: pipe() failed\n");
		return 1;
	}
	int pid = fork();
	if (pid == 0) {                 /* child: stdout -> pipe write end */
		close(fd[0]);
		dup2(fd[1], 1);
		close(fd[1]);
		printf("hello through dup2 + pipe\n");
		fflush(stdout);
		_exit(0);
	}
	/* parent: wait for the read end to become readable, then read it */
	close(fd[1]);
	struct pollfd pf = { fd[0], POLLIN, 0 };
	int pr = poll(&pf, 1, 2000);
	char buf[128];
	int n = read(fd[0], buf, sizeof buf - 1);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	printf("pipetest: poll=%d revents=%d, read %d bytes: %s\n", pr, pf.revents, n, buf);
	fflush(stdout);
	return 0;
}
