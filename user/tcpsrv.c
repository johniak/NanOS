/*
 * tcpsrv — a minimal TCP echo server (FAZA H diagnostic). Exercises the SERVER side of the
 * stack the exact way inetd does — socket/setsockopt(SO_REUSEADDR)/bind(INADDR_ANY)/listen/
 * accept/read/write — but in the FOREGROUND, printing each step's return value to the console,
 * so a failure in the passive-open path is visible (inetd hides it behind daemon()+syslog).
 *
 * Usage: tcpsrv [port]   (default 7777). Serves one connection (echoes a line back) then exits.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(int argc, char** argv) {
	int port = (argc > 1) ? atoi(argv[1]) : 7777;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	printf("tcpsrv: socket() = %d\n", fd);
	if (fd < 0) { printf("  errno=%d\n", errno); return 1; }

	int one = 1;
	int r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	printf("tcpsrv: setsockopt(SO_REUSEADDR) = %d (errno=%d)\n", r, r < 0 ? errno : 0);

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short) port);
	sa.sin_addr.s_addr = INADDR_ANY;
	r = bind(fd, (struct sockaddr*) &sa, sizeof sa);
	printf("tcpsrv: bind(0.0.0.0:%d) = %d (errno=%d)\n", port, r, r < 0 ? errno : 0);
	if (r < 0) return 1;

	r = listen(fd, 5);
	printf("tcpsrv: listen(5) = %d (errno=%d)\n", r, r < 0 ? errno : 0);
	if (r < 0) return 1;

	printf("tcpsrv: listening on :%d, waiting for accept...\n", port);
	struct sockaddr_in ca;
	socklen_t cl = sizeof ca;
	int c = accept(fd, (struct sockaddr*) &ca, &cl);
	printf("tcpsrv: accept() = %d (errno=%d)\n", c, c < 0 ? errno : 0);
	if (c < 0) return 1;

	char buf[256];
	int n = read(c, buf, sizeof buf);
	printf("tcpsrv: read() = %d\n", n);
	if (n > 0) {
		write(c, buf, n);              /* echo it back */
		printf("tcpsrv: echoed %d bytes\n", n);
	}
	close(c);
	close(fd);
	printf("tcpsrv: done\n");
	return 0;
}
