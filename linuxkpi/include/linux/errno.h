/*
 * linuxkpi/include/linux/errno.h — the errno constants Linux source uses (subset).
 *
 * IMPORTANT host-build note: glibc's <errno.h> itself does `#include <linux/errno.h>`, and
 * with -Ilinuxkpi/include on the path that resolves HERE. So under the host doctest build we
 * must NOT shadow the system's full errno set — chain to the real kernel header with
 * include_next. In the freestanding kext build (no system headers) we provide the subset.
 */
#ifndef _LINUXKPI_LINUX_ERRNO_H
#define _LINUXKPI_LINUX_ERRNO_H

#ifdef NANOS_HOST_TEST
#include_next <linux/errno.h>
#else

#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define E2BIG    7
#define ENOEXEC  8
#define EBADF    9
#define EAGAIN   11
#define ENOMEM   12
#define EACCES   13
#define EFAULT   14
#define EBUSY    16
#define EEXIST   17
#define ENODEV   19
#define EINVAL   22
#define EDEADLK  35
#define ERESTARTSYS 512
#define ENOTSUPP 524
#define ENOSPC   28
#define EROFS    30
#define ERANGE   34
#define ENOSYS   38
#define ENODATA  61
#define EPROTO   71
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define ETIME 62
#define EALREADY 114
#define ETIMEDOUT 110
#define EREMOTEIO 121
#define EPROBE_DEFER 517

#endif /* NANOS_HOST_TEST */
#endif /* _LINUXKPI_LINUX_ERRNO_H */
