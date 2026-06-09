/*
 * cwd.c — a userland current-working-directory layer.
 *
 * The kernel is cwd-free (every path it receives is absolute). sbase ls, however,
 * chdir()s into a directory and then stats entries by their relative name. We keep
 * the cwd here in userland and expand relative paths to absolute (nx_resolve),
 * which the glue's open/stat/lstat/opendir call before trapping to the kernel.
 */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include "SyscallNr.h"

static inline int sys3(int nr, int a, int b, int c) {
	int r;
	__asm__ __volatile__("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}

static char g_cwd[256] = "/";

/* Expand `path` to a clean absolute path in `out` (>= 256 bytes). Relative paths are
 * joined onto the current directory, then normalised: empty and "." components are
 * dropped and ".." pops the previous component (e.g. "." -> the cwd, "./dev" -> "/dev").
 * Without this, "ls ." would ask the kernel for a child literally named ".". */
void nx_resolve(const char* path, char* out) {
	char joined[512];
	if (path[0] == '/') {
		strncpy(joined, path, 511);
		joined[511] = 0;
	} else {
		strncpy(joined, g_cwd, 511);
		joined[511] = 0;
		unsigned l = strlen(joined);
		if (l == 0 || joined[l - 1] != '/') {
			if (l < 511) { joined[l++] = '/'; joined[l] = 0; }
		}
		strncat(joined, path, 511 - strlen(joined));
	}

	/* Split on '/', dropping "" and ".", popping on "..". */
	const char* seg[64];
	int seglen[64];
	int n = 0;
	const char* p = joined;
	while (*p) {
		while (*p == '/') p++;
		if (!*p) break;
		const char* s = p;
		while (*p && *p != '/') p++;
		int len = (int) (p - s);
		if (len == 1 && s[0] == '.') continue;
		if (len == 2 && s[0] == '.' && s[1] == '.') { if (n > 0) n--; continue; }
		if (n < 64) { seg[n] = s; seglen[n] = len; n++; }
	}

	char* o = out;
	char* end = out + 255;
	if (n == 0) { out[0] = '/'; out[1] = 0; return; }
	for (int i = 0; i < n && o < end; i++) {
		*o++ = '/';
		for (int k = 0; k < seglen[i] && o < end; k++)
			*o++ = seg[i][k];
	}
	*o = 0;
}

int chdir(const char* path) {
	char tmp[256];
	nx_resolve(path, tmp);
	/* Validate the target before committing: POSIX chdir must fail (not silently succeed)
	 * if the path does not exist or is not a directory. */
	struct stat st;
	if (stat(tmp, &st) != 0) {
		errno = ENOENT;
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}
	strcpy(g_cwd, tmp);
	return 0;
}

char* getcwd(char* buf, size_t size) {
	strncpy(buf, g_cwd, size);
	if (size) buf[size - 1] = 0;
	return buf;
}

/* readlink: read a symbolic link's target via the kernel (SYS_readlink). Returns the byte
 * count (no NUL), or -1/errno; -EINVAL if the path is not a symlink. */
ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
	char abs[256];
	nx_resolve(path, abs);
	int r = sys3(SYS_readlink, (int) abs, (int) buf, (int) bufsiz);
	if (r < 0) { errno = -r; return -1; }
	return r;
}
