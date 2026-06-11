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

/* picolibc's <sys/resource.h> ships only getrusage/struct rusage, not the rlimit surface.
 * Mirror the minimal struct the getrlimit/setrlimit glue (posixstubs.c) implements; the SDK
 * sysroot adds the same declarations (+ RLIMIT_*) for external ports. */
struct rlimit { unsigned long rlim_cur; unsigned long rlim_max; };
#ifndef RLIM_INFINITY
#define RLIM_INFINITY (~0UL)
#endif

/* getprogname/setprogname (BSD): picolibc doesn't declare them; the SDK sysroot adds the same
 * decls for external ports. The crt0 hook __nx_set_progname seeds it from argv[0]. */
const char* getprogname(void);
void setprogname(const char* p);
void __nx_set_progname(const char* argv0);

#endif /* NX_COMPAT_DECLS_H */
