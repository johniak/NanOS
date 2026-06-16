/*
 * limits.h — thin overlay over the toolchain's <limits.h>. Pulls in the real gcc + picolibc
 * limits, then guarantees the POSIX path/name limits that picolibc gates behind feature macros
 * the SDK port build doesn't set. Apps (wget) use PATH_MAX/NAME_MAX unconditionally.
 */
#ifndef _NX_LIMITS_OVERLAY_H
#define _NX_LIMITS_OVERLAY_H

#include_next <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

/* POSIX threads / semaphore limits the vendored musl pthread sources (pthread_key_create.c,
 * sem_*.c) need. picolibc's <limits.h> does not define them; values are musl 1.2.5's. */
#ifndef PTHREAD_KEYS_MAX
#define PTHREAD_KEYS_MAX 128
#endif
#ifndef PTHREAD_DESTRUCTOR_ITERATIONS
#define PTHREAD_DESTRUCTOR_ITERATIONS 4
#endif
#ifndef SEM_VALUE_MAX
#define SEM_VALUE_MAX 0x7fffffff
#endif

#endif /* _NX_LIMITS_OVERLAY_H */
