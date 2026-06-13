/*
 * sys/ioctl.h — NanOS userland (picolibc has no <sys/ioctl.h>). The terminal request
 * numbers (TCGETS/TIOCGWINSZ/TIOCGPGRP/...) and struct winsize live in <sys/termios.h>;
 * pull them in (as Linux's <sys/ioctl.h> effectively does) and add the ioctl() prototype
 * plus a few extra request codes some programs reference.
 */
#ifndef _NANOS_SYS_IOCTL_H
#define _NANOS_SYS_IOCTL_H

#include <sys/termios.h>   /* TCGETS/TIOCGWINSZ/TIOCGPGRP/... + struct winsize */

/* Present so callers compile; the kernel returns -EINVAL for ones it does not implement. */
#ifndef TIOCSCTTY
#define TIOCSCTTY  0x540E
#endif
#ifndef TIOCNOTTY
#define TIOCNOTTY  0x5422
#endif
#ifndef FIONREAD
#define FIONREAD   0x541B
#endif

/* Socket ioctls (Linux SIOC*) used by ifconfig/udhcpc; handled in kernel/Syscall.cpp netIoctl. */
#define SIOCGIFCONF    0x8912
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891b
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCADDRT      0x890B
#define SIOCDELRT      0x890C

int ioctl(int fd, unsigned long request, ...);

#endif /* _NANOS_SYS_IOCTL_H */
