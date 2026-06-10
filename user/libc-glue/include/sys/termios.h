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
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

/* c_iflag (Linux values; the kernel interprets ICRNL/IXON, the rest are stored) */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IUCLC   0x0200
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000
#define IMAXBEL 0x2000
#define IUTF8   0x4000
/* c_oflag */
#define OPOST   0x0001
#define OLCUC   0x0002
#define ONLCR   0x0004
#define OCRNL   0x0008
#define ONOCR   0x0010
#define ONLRET  0x0020
#define OFILL   0x0040
#define OFDEL   0x0080
/* c_cflag */
#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define CREAD   0x0080
#define PARENB  0x0100
#define PARODD  0x0200
#define HUPCL   0x0400
#define CLOCAL  0x0800
/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define XCASE   0x0004
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define ECHOCTL 0x0200
#define ECHOPRT 0x0400
#define ECHOKE  0x0800
#define FLUSHO  0x1000
#define PENDIN  0x4000
#define IEXTEN  0x8000
/* baud rates (values are the CBAUD bit patterns; informational here) */
#define B0      0x0000
#define B50     0x0001
#define B75     0x0002
#define B110    0x0003
#define B134    0x0004
#define B150    0x0005
#define B200    0x0006
#define B300    0x0007
#define B600    0x0008
#define B1200   0x0009
#define B1800   0x000A
#define B2400   0x000B
#define B4800   0x000C
#define B9600   0x000D
#define B19200  0x000E
#define B38400  0x000F

/* tcsetattr actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2
/* tcflush queue selectors */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
/* tcflow actions (Linux values) */
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

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
int tcflow(int fd, int action);
int tcdrain(int fd);
int tcsendbreak(int fd, int duration);
void cfmakeraw(struct termios* t);
speed_t cfgetispeed(const struct termios* t);
speed_t cfgetospeed(const struct termios* t);
int cfsetispeed(struct termios* t, speed_t s);
int cfsetospeed(struct termios* t, speed_t s);
#ifdef __cplusplus
}
#endif

#endif
