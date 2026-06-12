/*
 * stdio_ext.c — the glibc/Solaris stdio-extension API over picolibc's tinystdio. picolibc only
 * ships __fpending; gnulib-based ports (wget) need __freading/__fwriting/__fpurge/etc. tinystdio
 * keeps read/write intent in `struct __file::flags` (__SRD/__SWR), which is all these need. It is
 * effectively unbuffered (the function-pointer device model), so buffer-size/purge are trivial.
 */
#include <stdio.h>
#include <stdio_ext.h>

/* `FILE` is `struct __file` (tinystdio); flags/unget are in the base struct. */

int __freading(FILE* fp)  { return (fp->flags & __SRD) != 0; }
int __fwriting(FILE* fp)  { return (fp->flags & __SWR) != 0; }
int __freadable(FILE* fp) { return (fp->flags & __SRD) != 0; }
int __fwritable(FILE* fp) { return (fp->flags & __SWR) != 0; }

/* tinystdio's streams are unbuffered (per-char put/get), so there is no stdio buffer to size or
 * line-buffer; __fpurge only has the one-char ungetc pushback + sticky EOF to discard. */
size_t __fbufsize(FILE* fp) { (void) fp; return 0; }
int    __flbf(FILE* fp)     { (void) fp; return 0; }

void __fpurge(FILE* fp) {
	fp->unget = 0;            /* drop any ungetc() pushback */
	fp->flags &= ~__SEOF;     /* clear sticky end-of-file */
}

/* Single-threaded userland: locking is a no-op; report internal locking. */
int __fsetlocking(FILE* fp, int type) {
	(void) fp; (void) type;
	return FSETLOCKING_INTERNAL;
}
