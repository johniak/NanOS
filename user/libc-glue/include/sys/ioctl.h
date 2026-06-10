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

int ioctl(int fd, unsigned long request, ...);

#endif /* _NANOS_SYS_IOCTL_H */
