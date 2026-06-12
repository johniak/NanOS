/*
 * stdio_ext.h — the glibc/Solaris stdio-extension API (__freading/__fwriting/__fpurge/...).
 * picolibc ships only __fpending; gnulib (and gnulib-based ports like wget) need the rest to
 * avoid its FILE-poking fallbacks. We implement them over picolibc's tinystdio (user/libc-glue/
 * stdio_ext.c), which exposes read/write intent in `struct __file::flags` (__SRD/__SWR).
 *
 * This header shadows picolibc's <stdio_ext.h> (libc-glue/include is earlier on the path); it
 * re-declares __fpending (still resolved from picolibc's libc) plus the family we add.
 */
#ifndef _STDIO_EXT_H
#define _STDIO_EXT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* __fsetlocking type values (glibc). */
#define FSETLOCKING_QUERY    0
#define FSETLOCKING_INTERNAL 1
#define FSETLOCKING_BYCALLER 2

size_t __fpending(FILE* fp);          /* bytes of pending output (picolibc) */
size_t __fbufsize(FILE* fp);          /* size of the stream's buffer */
int    __freading(FILE* fp);          /* nonzero if stream is in read mode / readable */
int    __fwriting(FILE* fp);          /* nonzero if stream is in write mode / writable */
int    __freadable(FILE* fp);         /* nonzero if reading is allowed */
int    __fwritable(FILE* fp);         /* nonzero if writing is allowed */
int    __flbf(FILE* fp);              /* nonzero if line-buffered */
void   __fpurge(FILE* fp);            /* discard buffered/pushed-back data */
int    __fsetlocking(FILE* fp, int type);

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_EXT_H */
