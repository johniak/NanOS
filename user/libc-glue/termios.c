/*
 * termios.c — userland termios over the kernel PTY ioctls (TCGETS/TCSETS). picolibc has no
 * working termios, so these are the canonical implementations programs link against.
 */
#include <sys/termios.h>

int ioctl(int fd, unsigned long request, ...);   /* from syscalls.c */

int tcgetattr(int fd, struct termios* t) {
	return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int actions, const struct termios* t) {
	/* TCSANOW/DRAIN/FLUSH map to TCSETS/TCSETSW/TCSETSF; we treat them alike (no output
	 * queue to drain in our line discipline). */
	unsigned long req = TCSETS;
	if (actions == TCSADRAIN) req = TCSETSW;
	else if (actions == TCSAFLUSH) req = TCSETSF;
	return ioctl(fd, req, (void*) t);
}

int tcflush(int fd, int queue) {
	(void) fd; (void) queue;
	return 0;   /* no persistent queues to flush in our buffers */
}

/* Put the termios into raw mode (no canonical line editing, no echo, no signal chars, no
 * output post-processing) — what TUIs and readline use. Mirrors glibc's cfmakeraw. */
void cfmakeraw(struct termios* t) {
	t->c_iflag &= ~(ICRNL | IXON);
	t->c_oflag &= ~OPOST;
	t->c_lflag &= ~(ISIG | ICANON | ECHO | ECHOE | ECHOK);
	t->c_cc[VMIN] = 1;
	t->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios* t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios* t) { return t->c_ospeed; }
int cfsetispeed(struct termios* t, speed_t s) { t->c_ispeed = s; return 0; }
int cfsetospeed(struct termios* t, speed_t s) { t->c_ospeed = s; return 0; }

/* Controlling-terminal foreground process group (job control), via the pty's TIOCSPGRP/
 * TIOCGPGRP ioctls. The shell calls tcsetpgrp() to hand the terminal to the job it runs in
 * the foreground, so the tty routes Ctrl+C/Ctrl+Z to that group. */
int tcsetpgrp(int fd, int pgrp) {
	return ioctl(fd, TIOCSPGRP, &pgrp);
}
int tcgetpgrp(int fd) {
	int p = 0;
	if (ioctl(fd, TIOCGPGRP, &p) < 0)
		return -1;
	return p;
}
