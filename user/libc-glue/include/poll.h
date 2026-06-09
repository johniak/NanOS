/* poll.h — picolibc has no <poll.h>; NanOS provides one for the terminal stack. Matches
 * the kernel's PollFd layout + bit values (kernel/Syscall.h). */
#ifndef _NX_POLL_H
#define _NX_POLL_H

struct pollfd {
	int fd;
	short events;
	short revents;
};
typedef unsigned long nfds_t;

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd* fds, nfds_t nfds, int timeout);
#ifdef __cplusplus
}
#endif

#endif
