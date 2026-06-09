/*
 * ptytest — Stage-2 proof: open both ends of the PTY and round-trip bytes through the
 * line discipline. Writing "hi\n" to the master commits a canonical line the slave reads;
 * writing from the slave surfaces on the master (with NL->CRLF). Mirrors how the terminal
 * emulator (master) and the shell (slave) will talk.
 */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

int main(void) {
	int m = open("/dev/ptmx", O_RDWR);
	int s = open("/dev/pts0", O_RDWR);
	if (m < 0 || s < 0) {
		printf("ptytest: open failed (m=%d s=%d)\n", m, s);
		return 1;
	}
	/* master -> line discipline -> slave (canonical: a full line) */
	write(m, "hi\n", 3);
	char buf[64];
	int n = read(s, buf, sizeof buf - 1);
	if (n < 0) n = 0;
	buf[n] = 0;
	printf("ptytest: slave read %d bytes: %s", n, buf);

	/* slave output -> master (OPOST/ONLCR turns \n into \r\n) */
	write(s, "OK\n", 3);
	n = read(m, buf, sizeof buf - 1);
	if (n < 0) n = 0;
	printf("ptytest: master read %d bytes (echo + output)\n", n);
	fflush(stdout);
	return 0;
}
