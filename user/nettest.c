/*
 * nettest — verifies the FAZA 11/12 libc networking surface through the REAL POSIX API (not raw
 * socketcall): getaddrinfo() resolves wp.pl, then socket()/connect()/send()/recv() fetch its
 * homepage over TCP. This is the exact API a precompiled Linux app (wget/ping) uses, so a clean
 * run proves the sysroot headers + libc.ndl socket wrappers + the stub resolver all work.
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

int main(void) {
	struct addrinfo hints, *res = 0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	int rc = getaddrinfo("wp.pl", "80", &hints, &res);
	if (rc != 0 || !res) { printf("nettest: getaddrinfo(wp.pl) failed: %s\n", gai_strerror(rc)); return 1; }
	struct sockaddr_in* sin = (struct sockaddr_in*) res->ai_addr;
	char ips[16];
	inet_ntop(AF_INET, &sin->sin_addr, ips, sizeof(ips));
	printf("nettest: getaddrinfo(wp.pl) -> %s:%u\n", ips, ntohs(sin->sin_port));

	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) { printf("nettest: socket() failed\n"); freeaddrinfo(res); return 1; }
	if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) { printf("nettest: connect() failed\n"); freeaddrinfo(res); return 1; }
	printf("nettest: connected to wp.pl:80\n");
	freeaddrinfo(res);

	const char* req = "GET / HTTP/1.0\r\nHost: wp.pl\r\n\r\n";
	send(fd, req, strlen(req), 0);
	char buf[256];
	int n = recv(fd, buf, sizeof(buf) - 1, 0);
	if (n > 0) {
		buf[n] = 0;
		char* nl = strchr(buf, '\r'); if (nl) *nl = 0;
		printf("nettest: HTTP reply: %s  <-- getaddrinfo + BSD sockets OK\n", buf);
	} else {
		printf("nettest: no HTTP reply (n=%d)\n", n);
	}
	close(fd);
	return 0;
}
