/*
 * linuxkpi/include/linux/poll.h — minimal poll surface for the shim. The DRM file ops
 * reference poll_table + EPOLL* but NanOS does not expose DRM render nodes to userspace
 * (the bridge drives KMS in-kernel), so these are inert.
 */
#ifndef _LINUXKPI_LINUX_POLL_H
#define _LINUXKPI_LINUX_POLL_H

#include <linux/types.h>
#include <linux/wait.h>

typedef unsigned __poll_t;
struct file;
struct poll_table_struct;
typedef struct poll_table_struct poll_table;

#define EPOLLIN   0x0001u
#define EPOLLOUT  0x0004u
#define EPOLLERR  0x0008u
#define EPOLLHUP  0x0010u
#define EPOLLRDNORM 0x0040u
#define EPOLLWRNORM 0x0100u

static inline void poll_wait(struct file *f, wait_queue_head_t *w, poll_table *p) { (void)f;(void)w;(void)p; }

#endif /* _LINUXKPI_LINUX_POLL_H */
