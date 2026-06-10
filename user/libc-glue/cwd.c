/*
 * cwd.c — chdir/getcwd/readlink wrappers.
 *
 * The current working directory lives in the KERNEL, per process (inherited by fork, kept
 * across execve), and the kernel resolves every relative path against it — exactly the Unix
 * model. So these are thin syscall wrappers; there is no userland cwd state to fake.
 */
#include <unistd.h>
#include <errno.h>
#include "SyscallNr.h"

static inline int sys3(int nr, int a, int b, int c) {
	int r;
	__asm__ __volatile__("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}

int chdir(const char* path) {
	int r = sys3(SYS_chdir, (int) path, 0, 0);
	if (r < 0) { errno = -r; return -1; }
	return 0;
}

char* getcwd(char* buf, size_t size) {
	int r = sys3(SYS_getcwd, (int) buf, (int) size, 0);
	if (r < 0) { errno = -r; return 0; }
	return buf;
}

/* readlink: read a symbolic link's target via the kernel (SYS_readlink). Returns the byte
 * count (no NUL), or -1/errno; -EINVAL if the path is not a symlink. */
ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
	int r = sys3(SYS_readlink, (int) path, (int) buf, (int) bufsiz);
	if (r < 0) { errno = -r; return -1; }
	return r;
}
