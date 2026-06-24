/*
 * Termios.h — kernel view of the terminal settings carried by TCGETS/TCSETS. The layout
 * and bit values MUST match the userland header (user/libc-glue/include/sys/termios.h),
 * since the ioctl copies the struct verbatim between user and kernel.
 */
#pragma once

namespace kernel {

static const int NCCS = 32;
struct Termios {
	unsigned c_iflag;
	unsigned c_oflag;
	unsigned c_cflag;
	unsigned c_lflag;
	unsigned char c_line;
	unsigned char c_cc[NCCS];
	unsigned c_ispeed;
	unsigned c_ospeed;
};

struct Winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

// c_cc indices
enum { VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VTIME = 5, VMIN = 6, VSUSP = 10 };
// c_iflag
enum { TI_ICRNL = 0x0100, TI_IXON = 0x0400 };
// c_oflag
enum { TO_OPOST = 0x0001, TO_ONLCR = 0x0004 };
// c_lflag
enum { TL_ISIG = 0x0001, TL_ICANON = 0x0002, TL_ECHO = 0x0008, TL_ECHOE = 0x0010, TL_ECHOK = 0x0020 };

// ioctl numbers (Linux i386)
enum {
	IOCTL_TCGETS = 0x5401, IOCTL_TCSETS = 0x5402, IOCTL_TCSETSW = 0x5403, IOCTL_TCSETSF = 0x5404,
	IOCTL_TIOCGWINSZ = 0x5413, IOCTL_TIOCSWINSZ = 0x5414,
	IOCTL_TIOCGPGRP = 0x540F, IOCTL_TIOCSPGRP = 0x5410,
	IOCTL_TIOCSCTTY = 0x540E,   // make this terminal the calling session's controlling tty
	IOCTL_TIOCPKT = 0x5420,    // pty packet mode (telnetd): master reads gain a status preamble
};

// Cooked-mode terminal defaults (ICRNL/ONLCR + ISIG/ICANON/ECHO + the usual control chars).
// Shared by the kernel console singleton and every VtConsole so they start identically.
inline void termiosInitCooked(Termios& t) {
	t.c_iflag = TI_ICRNL | TI_IXON;
	t.c_oflag = TO_OPOST | TO_ONLCR;
	t.c_cflag = 0;
	t.c_lflag = TL_ISIG | TL_ICANON | TL_ECHO | TL_ECHOE | TL_ECHOK;
	t.c_line = 0;
	for (int i = 0; i < NCCS; i++) t.c_cc[i] = 0;
	t.c_cc[VINTR] = 3; t.c_cc[VQUIT] = 28; t.c_cc[VERASE] = 0x7f;
	t.c_cc[VKILL] = 21; t.c_cc[VEOF] = 4; t.c_cc[VSUSP] = 26;
	t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
	t.c_ispeed = t.c_ospeed = 0;
}

// TIOCPKT status-byte bits (the preamble of each packet-mode master read). We only generate
// TIOCPKT_DATA(0) for ordinary data; the flush/flow bits exist for callers that test them.
enum {
	TIOCPKT_DATA = 0x00, TIOCPKT_FLUSHREAD = 0x01, TIOCPKT_FLUSHWRITE = 0x02,
	TIOCPKT_STOP = 0x04, TIOCPKT_START = 0x08, TIOCPKT_NOSTOP = 0x10,
	TIOCPKT_DOSTOP = 0x20, TIOCPKT_IOCTL = 0x40,
};

}  // namespace kernel
