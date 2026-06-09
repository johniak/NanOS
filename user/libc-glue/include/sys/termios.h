/*
 * sys/termios.h — NanOS termios. picolibc's <termios.h> just #includes <sys/termios.h>,
 * which doesn't exist on this target, so we provide it here (this dir is on the userland
 * include path). The layout + bit values match Linux/glibc, and the SAME numeric values
 * are used by the kernel PTY's TCGETS/TCSETS/TIOCGWINSZ ioctls (kernel/Termios.h).
 */
#ifndef _NX_SYS_TERMIOS_H
#define _NX_SYS_TERMIOS_H

typedef unsigned char  cc_t;
typedef unsigned int   speed_t;
typedef unsigned int   tcflag_t;

#define NCCS 32
struct termios {
	tcflag_t c_iflag;
	tcflag_t c_oflag;
	tcflag_t c_cflag;
	tcflag_t c_lflag;
	cc_t     c_line;
	cc_t     c_cc[NCCS];
	speed_t  c_ispeed;
	speed_t  c_ospeed;
};

/* c_cc indices (Linux) */
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VTIME  5
#define VMIN   6
#define VSUSP  10

/* c_iflag */
#define ICRNL  0x0100
#define IXON   0x0400
/* c_oflag */
#define OPOST  0x0001
#define ONLCR  0x0004
/* c_lflag */
#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0008
#define ECHOE  0x0010
#define ECHOK  0x0020
/* c_cflag (informational here) */
#define CS8    0x0030
#define CREAD  0x0080
#define CLOCAL 0x0800

/* tcsetattr actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2
/* tcflush queue selectors */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

/* ioctls (Linux i386 values) */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#ifdef __cplusplus
extern "C" {
#endif
int tcgetattr(int fd, struct termios* t);
int tcsetattr(int fd, int actions, const struct termios* t);
int tcflush(int fd, int queue);
void cfmakeraw(struct termios* t);
speed_t cfgetispeed(const struct termios* t);
speed_t cfgetospeed(const struct termios* t);
int cfsetispeed(struct termios* t, speed_t s);
int cfsetospeed(struct termios* t, speed_t s);
#ifdef __cplusplus
}
#endif

#endif
