/*
 * net.c — the socket layer of libc.ndl: BSD socket calls over the NanOS socketcall(2) ABI
 * (int 0x80), plus inet_* address conversion and select(). The kernel side (FAZA 7-9) does the
 * real work; this is the thin userland binding Linux apps link against.
 */
#include "SyscallNr.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

static inline int sys3(int nr, int a, int b, int c) {
	int r;
	__asm__ __volatile__("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}
static int reterr(int r) { if (r < 0) { errno = -r; return -1; } return r; }
static int scall(int call, unsigned long* a) { return sys3(SYS_socketcall, call, (int) (long) a, 0); }

int socket(int domain, int type, int protocol) {
	unsigned long a[3] = { (unsigned long) domain, (unsigned long) type, (unsigned long) protocol };
	return reterr(scall(SC_SOCKET, a));
}
int socketpair(int domain, int type, int protocol, int sv[2]) {
	unsigned long a[4] = { (unsigned long) domain, (unsigned long) type, (unsigned long) protocol, (unsigned long) sv };
	return reterr(scall(SC_SOCKETPAIR, a));
}
int bind(int fd, const struct sockaddr* addr, socklen_t len) {
	unsigned long a[3] = { (unsigned long) fd, (unsigned long) addr, len };
	return reterr(scall(SC_BIND, a));
}
int connect(int fd, const struct sockaddr* addr, socklen_t len) {
	unsigned long a[3] = { (unsigned long) fd, (unsigned long) addr, len };
	return reterr(scall(SC_CONNECT, a));
}
int listen(int fd, int backlog) {
	unsigned long a[2] = { (unsigned long) fd, (unsigned long) backlog };
	return reterr(scall(SC_LISTEN, a));
}
int accept(int fd, struct sockaddr* addr, socklen_t* len) {
	unsigned long a[4] = { (unsigned long) fd, (unsigned long) addr, (unsigned long) len, 0 };
	return reterr(scall(SC_ACCEPT, a));
}
int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags) {
	unsigned long a[4] = { (unsigned long) fd, (unsigned long) addr, (unsigned long) len, (unsigned long) flags };
	return reterr(scall(SC_ACCEPT4, a));
}
int getsockname(int fd, struct sockaddr* addr, socklen_t* len) {
	unsigned long a[3] = { (unsigned long) fd, (unsigned long) addr, (unsigned long) len };
	return reterr(scall(SC_GETSOCKNAME, a));
}
int getpeername(int fd, struct sockaddr* addr, socklen_t* len) {
	unsigned long a[3] = { (unsigned long) fd, (unsigned long) addr, (unsigned long) len };
	return reterr(scall(SC_GETPEERNAME, a));
}
int getsockopt(int fd, int level, int name, void* val, socklen_t* len) {
	unsigned long a[5] = { (unsigned long) fd, (unsigned long) level, (unsigned long) name, (unsigned long) val, (unsigned long) len };
	return reterr(scall(SC_GETSOCKOPT, a));
}
int setsockopt(int fd, int level, int name, const void* val, socklen_t len) {
	unsigned long a[5] = { (unsigned long) fd, (unsigned long) level, (unsigned long) name, (unsigned long) val, len };
	return reterr(scall(SC_SETSOCKOPT, a));
}
int shutdown(int fd, int how) {
	unsigned long a[2] = { (unsigned long) fd, (unsigned long) how };
	return reterr(scall(SC_SHUTDOWN, a));
}
ssize_t send(int fd, const void* buf, size_t len, int flags) {
	unsigned long a[4] = { (unsigned long) fd, (unsigned long) buf, len, (unsigned long) flags };
	return reterr(scall(SC_SEND, a));
}
ssize_t recv(int fd, void* buf, size_t len, int flags) {
	unsigned long a[4] = { (unsigned long) fd, (unsigned long) buf, len, (unsigned long) flags };
	return reterr(scall(SC_RECV, a));
}
ssize_t sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* addr, socklen_t alen) {
	unsigned long a[6] = { (unsigned long) fd, (unsigned long) buf, len, (unsigned long) flags, (unsigned long) addr, alen };
	return reterr(scall(SC_SENDTO, a));
}
ssize_t recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* addr, socklen_t* alen) {
	unsigned long a[6] = { (unsigned long) fd, (unsigned long) buf, len, (unsigned long) flags, (unsigned long) addr, (unsigned long) alen };
	return reterr(scall(SC_RECVFROM, a));
}
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags) {
	unsigned long a[3] = { (unsigned long) fd, (unsigned long) msg, (unsigned long) flags };
	return reterr(scall(SC_SENDMSG, a));
}
ssize_t recvmsg(int fd, struct msghdr* msg, int flags) {
	unsigned long a[3] = { (unsigned long) fd, (unsigned long) msg, (unsigned long) flags };
	return reterr(scall(SC_RECVMSG, a));
}

/* (select() lives in syscalls.c.) */

/* ---- inet_* address conversion ---- */
in_addr_t inet_addr(const char* cp) {
	struct in_addr a;
	return inet_aton(cp, &a) ? a.s_addr : INADDR_NONE;
}
int inet_aton(const char* cp, struct in_addr* inp) {
	unsigned parts[4]; int n = 0;
	const char* p = cp;
	for (; n < 4; n++) {
		if (*p < '0' || *p > '9') return 0;
		unsigned v = 0;
		while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned) (*p - '0'); if (v > 255) return 0; p++; }
		parts[n] = v;
		if (n < 3) { if (*p != '.') return 0; p++; }
	}
	if (*p != 0) return 0;
	if (inp) inp->s_addr = htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
	return 1;
}
int inet_pton(int af, const char* src, void* dst) {
	if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
	struct in_addr a;
	if (!inet_aton(src, &a)) return 0;
	*(in_addr_t*) dst = a.s_addr;
	return 1;
}
char* inet_ntoa(struct in_addr in) {
	static char buf[16];
	unsigned long h = ntohl(in.s_addr);
	snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu", (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff);
	return buf;
}
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
	if (af != AF_INET) { errno = EAFNOSUPPORT; return 0; }
	unsigned long h = ntohl(*(const in_addr_t*) src);
	char tmp[16];
	snprintf(tmp, sizeof(tmp), "%lu.%lu.%lu.%lu", (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff);
	if ((socklen_t) strlen(tmp) + 1 > size) { errno = 28 /*ENOSPC*/; return 0; }
	strcpy(dst, tmp);
	return dst;
}
