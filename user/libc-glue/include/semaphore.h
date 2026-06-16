/*
 * semaphore.h — the public POSIX semaphore header for the NanOS userland.
 *
 * picolibc ships no <semaphore.h> for i686-elf, so this is the sole definition the vendored
 * musl sem_* sources (user/libc-glue/pthread/sem_*.c) and programs see. It is musl 1.2.5's
 * <include/semaphore.h> with the header-generation machinery (bits/alltypes.h + __NEED_*)
 * inlined for i386 (sizeof(long)==4 ⇒ sem_t has 4 ints) and the _REDIR_TIME64 redirect
 * dropped (NanOS time_t is 32-bit). The sem_t layout is byte-for-byte musl's i386 layout
 * (__val[0]=count, [1]=waiters, [2]=pshared/private flag) — do not change the array size.
 */
#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <features.h>
#include <time.h>   /* struct timespec */

#define SEM_FAILED ((sem_t *)0)

typedef struct {
	volatile int __val[4];
} sem_t;

int    sem_close(sem_t *);
int    sem_destroy(sem_t *);
int    sem_getvalue(sem_t *__restrict, int *__restrict);
int    sem_init(sem_t *, int, unsigned);
sem_t *sem_open(const char *, int, ...);
int    sem_post(sem_t *);
int    sem_timedwait(sem_t *__restrict, const struct timespec *__restrict);
int    sem_trywait(sem_t *);
int    sem_unlink(const char *);
int    sem_wait(sem_t *);

#ifdef __cplusplus
}
#endif
#endif
