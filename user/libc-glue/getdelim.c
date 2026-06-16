/*
 * getdelim.c — POSIX getdelim/getline over picolibc's tinystdio.
 *
 * WHY: picolibc's tinystdio getdelim returns 0 at end-of-file, but POSIX (and the software that
 * relies on it) requires getdelim to return the byte count (>0) for a line, or -1 on EOF/error —
 * NEVER 0. git's strbuf_getwholeline (strbuf.c) literally `assert(r == -1)` after a non-positive
 * return, so a 0 from getdelim aborts git's subprocesses (SIGABRT) and corrupts pipe parsing
 * (e.g. "repack: Expecting full hex object ID lines only from pack-objects"). libc-glue objects
 * link before -lc, so this definition overrides picolibc's and gives ports correct semantics.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>   /* ssize_t */

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
	if (!lineptr || !n || !stream) {
		errno = EINVAL;
		return -1;
	}
	if (!*lineptr || *n == 0) {
		*n = 128;
		char *p = (char *) realloc(*lineptr, *n);   /* realloc(NULL, ..) == malloc */
		if (!p) { errno = ENOMEM; return -1; }
		*lineptr = p;
	}

	size_t pos = 0;
	for (;;) {
		int c = getc(stream);
		if (c == EOF) {
			if (pos == 0)
				return -1;            /* nothing read before EOF -> POSIX returns -1 */
			break;
		}
		if (pos + 1 >= *n) {              /* keep room for the NUL terminator */
			size_t nn = *n * 2;
			char *p = (char *) realloc(*lineptr, nn);
			if (!p) { errno = ENOMEM; return -1; }
			*lineptr = p;
			*n = nn;
		}
		(*lineptr)[pos++] = (char) c;
		if (c == delim)
			break;
	}
	(*lineptr)[pos] = '\0';
	return (ssize_t) pos;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
	return getdelim(lineptr, n, '\n', stream);
}
