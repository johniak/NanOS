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

static char g_cwd[256] = "/";

/* Expand `path` to an absolute path in `out` (>= 256 bytes). Relative paths are
 * joined onto the current directory; no "."/".." normalisation (not needed). */
void nx_resolve(const char* path, char* out) {
	if (path[0] == '/') {
		strncpy(out, path, 255);
		out[255] = 0;
		return;
	}
	strncpy(out, g_cwd, 255);
	out[255] = 0;
	unsigned l = strlen(out);
	if (l == 0 || out[l - 1] != '/') {
		if (l < 255) { out[l++] = '/'; out[l] = 0; }
	}
	strncat(out, path, 255 - strlen(out));
}

int chdir(const char* path) {
	char tmp[256];
	nx_resolve(path, tmp);
	strcpy(g_cwd, tmp);
	return 0;
}

char* getcwd(char* buf, size_t size) {
	strncpy(buf, g_cwd, size);
	if (size) buf[size - 1] = 0;
	return buf;
}

/* NanOS has no symlinks; readlink always fails (ls only calls it for S_ISLNK). */
ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
	(void) path; (void) buf; (void) bufsiz;
	errno = EINVAL;
	return -1;
}
