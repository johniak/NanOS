/* sys/eventfd.h — NanOS libc-glue. eventfd2(2): a 64-bit counter fd used by libuv/Chromium as a
 * self-pipe wakeup. Flag values match Linux (the kernel translates EFD_NONBLOCK/EFD_CLOEXEC to
 * its internal fd flags). */
#ifndef _SYS_EVENTFD_H
#define _SYS_EVENTFD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t eventfd_t;

enum {
	EFD_SEMAPHORE = 0x00001,   /* read yields 1 and decrements, instead of draining the whole count */
	EFD_CLOEXEC   = 0x80000,   /* == O_CLOEXEC: close on execve */
	EFD_NONBLOCK  = 0x00800    /* == O_NONBLOCK: read/write return EAGAIN instead of blocking */
};

int eventfd(unsigned int initval, int flags);

/* glibc convenience helpers over read()/write() of the 8-byte counter. */
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_EVENTFD_H */
