/*
 * sys/ioctl.h — NanOS userland (picolibc has no <sys/ioctl.h>). The terminal request
 * numbers (TCGETS/TIOCGWINSZ/TIOCGPGRP/...) and struct winsize live in <sys/termios.h>;
 * pull them in (as Linux's <sys/ioctl.h> effectively does) and add the ioctl() prototype
 * plus a few extra request codes some programs reference.
 */
#ifndef _NANOS_SYS_IOCTL_H
#define _NANOS_SYS_IOCTL_H

#include <sys/termios.h>
#include <asm/ioctl.h>   /* _IOC/_IOR/_IOW/_IOWR for DRM ioctl macros */   /* TCGETS/TIOCGWINSZ/TIOCGPGRP/... + struct winsize */

/* Present so callers compile; the kernel returns -EINVAL for ones it does not implement. */
#ifndef TIOCSCTTY
#define TIOCSCTTY  0x540E
#endif
#ifndef TIOCGPTN
#define TIOCGPTN   0x80045430   /* get the pty slave number (libuv openpty) */
#define TIOCSPTLCK 0x40045431   /* (un)lock the pty slave */
#endif
#ifndef TIOCNOTTY
#define TIOCNOTTY  0x5422
#endif
#ifndef FIONREAD
#define FIONREAD   0x541B
#endif
/* Terminal line-control ioctls (telnet client's sys_bsd.c flushes the tty with TCFLSH). The
 * kernel pty/console may treat these as no-ops; they only need to exist to compile + run. */
#ifndef TCSBRK
#define TCSBRK     0x5409
#define TCXONC     0x540A
#define TCFLSH     0x540B
#endif
#ifndef FIONBIO
#define FIONBIO    0x5421   /* set/clear non-blocking I/O (arg: int*) */
#endif
/* Pseudo-terminal packet mode (telnetd turns it on to learn about tty flushes). The kernel pty
 * may treat TIOCPKT as a no-op; telnetd ignores the ioctl's return value, so these only need to
 * exist for it to compile + run. The status byte the master reads in packet mode carries these
 * bits. */
#ifndef TIOCPKT
#define TIOCPKT             0x5420
#define TIOCPKT_DATA        0x00
#define TIOCPKT_FLUSHREAD   0x01
#define TIOCPKT_FLUSHWRITE  0x02
#define TIOCPKT_STOP        0x04
#define TIOCPKT_START       0x08
#define TIOCPKT_NOSTOP      0x10
#define TIOCPKT_DOSTOP      0x20
#define TIOCPKT_IOCTL       0x40
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

/* extern "C" so C++ ports (node/V8, libuv tty) reference the unmangled ioctl libc-glue defines with
 * C linkage — otherwise the C++ call mangles to _Z5ioctlimz and links to address 0. */
#ifdef __cplusplus
extern "C"
#endif
int ioctl(int fd, unsigned long request, ...);

#endif /* _NANOS_SYS_IOCTL_H */
