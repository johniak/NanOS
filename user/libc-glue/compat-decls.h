/*
 * compat-decls.h — declarations picolibc omits for the i686-elf target but that
 * the verbatim sbase sources reference. Force-included via -include so the vendored
 * programs stay unmodified. The definitions live in user/libc-glue/syscalls.c.
 */
#ifndef NX_COMPAT_DECLS_H
#define NX_COMPAT_DECLS_H

struct stat;
int lstat(const char* path, struct stat* buf);

/* picolibc gates the POSIX timer API behind a feature macro that our freestanding
 * build doesn't set, so it declares neither clock_gettime/nanosleep nor the CLOCK_*
 * ids — but it DOES define struct timespec/clockid_t. Expose them here (impl in
 * user/libc-glue/syscalls.c). NanOS has a single monotonic clock, so the id is moot. */
#include <time.h>
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
int clock_gettime(clockid_t clk, struct timespec* tp);
int nanosleep(const struct timespec* req, struct timespec* rem);

#endif /* NX_COMPAT_DECLS_H */
