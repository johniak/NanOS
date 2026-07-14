/* sys/epoll.h — NanOS libc-glue. Level-triggered epoll over the kernel interest set, the readiness
 * mechanism libuv/Chromium's message pump blocks in. EPOLLET/EPOLLONESHOT are accepted but not
 * honored (NanOS epoll is level-triggered only); see docs/en/electron-platform.md. */
#ifndef _SYS_EPOLL_H
#define _SYS_EPOLL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event bits — identical to the poll(2) POLL* values (the kernel maps them 1:1). */
enum {
	EPOLLIN      = 0x001,
	EPOLLPRI     = 0x002,
	EPOLLOUT     = 0x004,
	EPOLLERR     = 0x008,
	EPOLLHUP     = 0x010,
	EPOLLRDHUP   = 0x2000,
	EPOLLONESHOT = 0x40000000,   /* accepted, not honored (level-triggered only) */
	EPOLLET      = 0x80000000    /* accepted, not honored (level-triggered only) */
};

/* epoll_ctl ops. */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* epoll_create1 flags. */
#define EPOLL_CLOEXEC 0x80000    /* == O_CLOEXEC */

typedef union epoll_data {
	void     *ptr;
	int       fd;
	uint32_t  u32;
	uint64_t  u64;
} epoll_data_t;

/* Linux x86_64 packs this to 12 bytes (4-byte events + 8-byte data, no padding); the kernel's
 * marshalling matches, so keep it packed here too. */
struct epoll_event {
	uint32_t     events;
	epoll_data_t data;
} __attribute__((packed));

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
/* epoll_pwait: epoll_wait with a signal mask swapped around the wait. NanOS has no per-wait sigmask
 * swap, so it ignores `sigmask` and behaves as epoll_wait (libuv passes NULL in practice). */
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const void *sigmask);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_EPOLL_H */
