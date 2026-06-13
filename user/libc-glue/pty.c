/*
 * pty.c — openpty/forkpty/login_tty over the NanOS kernel pseudo-terminal. The kernel exposes one
 * pty pair: /dev/ptmx (master) + /dev/pts0 (slave) (see drivers/Pty.h, kernel/Kernel.cpp). These
 * wrap it the BSD way so inetutils telnetd (and future sshd/script) get a real controlling tty for
 * the login shell.
 *
 * NOTE: a single kernel pty pair means ONE pty session at a time (enough for one telnet login —
 * the FAZA H acceptance). Concurrent sessions need dynamic pty allocation (multiple pairs) in the
 * kernel; that is a separate kernel change, not a libc concern.
 */
#include <pty.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>

int login_tty(int fd) {
	setsid();                         /* new session; the slave becomes our controlling terminal */
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) close(fd);
	return 0;
}

int openpty(int* amaster, int* aslave, char* name,
            const struct termios* termp, const struct winsize* winp) {
	int m = open("/dev/ptmx", O_RDWR);
	if (m < 0) return -1;
	int s = open("/dev/pts0", O_RDWR);
	if (s < 0) { close(m); return -1; }
	if (termp) tcsetattr(s, TCSANOW, termp);
	if (winp)  ioctl(s, TIOCSWINSZ, (void*) winp);
	if (amaster) *amaster = m;
	if (aslave)  *aslave  = s;
	if (name)    strcpy(name, "/dev/pts0");
	return 0;
}

int forkpty(int* amaster, char* name,
            const struct termios* termp, const struct winsize* winp) {
	int m = -1, s = -1;
	if (openpty(&m, &s, name, termp, winp) < 0) return -1;
	int pid = fork();
	if (pid < 0) { close(m); close(s); return -1; }
	if (pid == 0) {                   /* child: slave becomes the controlling tty */
		close(m);
		login_tty(s);
		return 0;
	}
	if (amaster) *amaster = m;        /* parent: keep the master, drop the slave */
	close(s);
	return pid;
}
