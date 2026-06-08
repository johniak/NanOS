/*
 * fstest — exercise the writable /tmp (tmpfs) end to end (Stage 4 of the Doom port).
 *
 * Uses the raw POSIX syscalls (open/write/read/close/mkdir) with a stack buffer so the
 * test exercises OUR open(O_CREAT)/write/read/mkdir path directly, not picolibc stdio
 * buffering. Writes a file under /tmp, reads it back, checks it, makes a directory, and
 * confirms the ext disk is still read-only. This is the path Doom uses to save config.
 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

int main(void) {
	const char* path = "/tmp/hello.txt";
	const char* msg = "doom was here\n";
	int mlen = (int) strlen(msg);

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { printf("fstest: open(w) FAILED\n"); return 1; }
	write(fd, msg, mlen);
	close(fd);

	char buf[64];
	fd = open(path, O_RDONLY);
	if (fd < 0) { printf("fstest: open(r) FAILED\n"); return 1; }
	int n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n < 0) n = 0;
	buf[n] = 0;

	printf("fstest: wrote %d bytes, read back: %s", mlen, buf);
	int ok = (strcmp(buf, msg) == 0);

	printf("fstest: mkdir /tmp/sub %s\n", mkdir("/tmp/sub", 0755) == 0 ? "OK" : "FAILED");

	int ro = open("/disks/main/nope.txt", O_WRONLY | O_CREAT, 0644);
	printf("fstest: write to ext disk %s (expected refused)\n", ro >= 0 ? "ALLOWED?!" : "refused");
	if (ro >= 0) close(ro);

	printf("fstest: %s\n", ok ? "OK (tmpfs read/write works)" : "FAIL");
	return ok ? 0 : 1;
}
