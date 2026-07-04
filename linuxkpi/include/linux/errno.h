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
#define ENOTTY   25
#define ENODATA  61
#define EPROTO   71
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define ETIME 62
#define EALREADY 114
#define ETIMEDOUT 110
#define EREMOTEIO 121
#define EPROBE_DEFER 517

/* Remainder of the canonical asm-generic errno set (values from asm-generic/errno{,-base}.h). i915
 * maps its firmware/uc load states onto several of these (ESTALE, ENOEXEC, ENOPKG, ...). Added whole
 * rather than one-at-a-time to end the whack-a-mole; none collide with the subset above. */
#define ECHILD        10
#define ENOTBLK       15
#define EXDEV         18
#define ENOTDIR       20
#define EISDIR        21
#define ENFILE        23
#define EMFILE        24
#define ETXTBSY       26
#define EFBIG         27
#define ESPIPE        29
#define EMLINK        31
#define EPIPE         32
#define EDOM          33
#define ENAMETOOLONG  36
#define ENOLCK        37
#define ENOTEMPTY     39
#define ELOOP         40
#define EWOULDBLOCK   EAGAIN
#define ENOMSG        42
#define EIDRM         43
#define ECHRNG        44
#define EBADRQC       56
#define EBADSLT       57
#define EBFONT        59
#define ENOSTR        60
#define ENONET        64
#define ENOPKG        65
#define EREMOTE       66
#define ENOLINK       67
#define ECOMM         70
#define EMULTIHOP     72
#define EDOTDOT       73
#define EBADMSG       74
#define ENOTUNIQ      76
#define EBADFD        77
#define EMSGSIZE      90
#define EPROTOTYPE    91
#define ENOPROTOOPT   92
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT  97
#define EADDRINUSE    98
#define EADDRNOTAVAIL 99
#define ENETDOWN      100
#define ENETUNREACH   101
#define ENETRESET     102
#define ECONNABORTED  103
#define ECONNRESET    104
#define ENOBUFS       105
#define EISCONN       106
#define ENOTCONN      107
#define ESHUTDOWN     108
#define ECONNREFUSED  111
#define EHOSTDOWN     112
#define EHOSTUNREACH  113
#define EINPROGRESS   115
#define ESTALE        116
#define EDQUOT        122
#define ENOMEDIUM     123
#define EMEDIUMTYPE   124
#define ECANCELED     125
#define ENOKEY        126

#endif /* NANOS_HOST_TEST */
#endif /* _LINUXKPI_LINUX_ERRNO_H */
