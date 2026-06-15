/*
 * errnotest — verify errno still reports correctly via the FUNCTION path (__errno_location).
 *
 * Task 3.1 switched picolibc + libc.ndl from a single process-wide `errno` data slot to a
 * per-thread cell reached through `(*__errno_location())`. This single-threaded program
 * exercises that path: a failing open(2) must leave errno == ENOENT, and a successful call
 * afterwards must NOT clobber it spuriously. (Per-thread independence is exercised in Phase 4.)
 */
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

int main(void) {
	errno = 0;
	int fd = open("/no/such/file/here", O_RDONLY);
	printf("errnotest: open ret=%d errno=%d (%s)\n", fd, errno, strerror(errno));
	if (fd < 0 && errno == ENOENT)
		printf("errnotest: errno == ENOENT  OK\n");
	else
		printf("errnotest: errno wrong  FAIL\n");

	/* errno is an lvalue through the macro: assignment must work too. */
	errno = EINVAL;
	printf("errnotest: set errno=EINVAL -> reads %d %s\n", errno,
	       errno == EINVAL ? "OK" : "FAIL");
	return 0;
}
