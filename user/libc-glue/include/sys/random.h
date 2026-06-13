/*
 * sys/random.h — getrandom(2) / getentropy(3) declarations for NanOS.
 *
 * Backed by the kernel CSPRNG (SYS_getrandom -> kernel/Csprng.*, seeded from RDRAND+jitter+RTC).
 * The wrappers live in libc-glue/syscalls.c; this header lets gnulib-based programs (wget) and
 * any crypto code that includes <sys/random.h> see a prototype instead of an implicit declaration.
 */
#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <sys/types.h>   /* ssize_t, size_t */

#ifdef __cplusplus
extern "C" {
#endif

/* getrandom() flags. Our kernel CSPRNG never blocks and is always seeded, so both are accepted
 * and ignored — getrandom always returns the requested count from the CSPRNG. */
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

ssize_t getrandom(void* buf, size_t buflen, unsigned int flags);
int     getentropy(void* buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_RANDOM_H */
