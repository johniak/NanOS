/*
 * unixtest — a ring-3 smoke test for AF_UNIX (FAZA G). Self-contained (raw int 0x80, no libc),
 * it exercises (1) socketpair(AF_UNIX, SOCK_STREAM) with a ping/pong both ways and (2) a named
 * stream socket: bind a path under /tmp, listen, connect (local — completes immediately), accept,
 * and exchange a message. Proves the userland -> int 0x80 -> socketcall -> net/Unix path.
 */
static inline int sys3(int nr, int a, int b, int c) {
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}
#define SYS_read  3
#define SYS_write 4
#define SYS_close 6
#define SYS_socketcall 102

#define SC_SOCKET     1
#define SC_BIND       2
#define SC_CONNECT    3
#define SC_LISTEN     4
#define SC_ACCEPT     5
#define SC_SOCKETPAIR 8

#define AF_UNIX 1
#define SOCK_STREAM 1

static int sock(int call, unsigned long* a) { return sys3(SYS_socketcall, call, (int) (long) a, 0); }
static void puts1(const char* s) { int n = 0; while (s[n]) n++; sys3(SYS_write, 1, (int) (long) s, n); }
static int eq(const char* a, const char* b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

int main(void) {
	int ok = 1;
	unsigned long a[6];
	char buf[32];

	/* 1) socketpair: bidirectional byte stream */
	int sv[2] = { -1, -1 };
	a[0] = AF_UNIX; a[1] = SOCK_STREAM; a[2] = 0; a[3] = (unsigned long) sv;
	if (sock(SC_SOCKETPAIR, a) < 0) { puts1("unixtest: socketpair FAIL\n"); return 1; }
	sys3(SYS_write, sv[0], (int) (long) "ping", 4);
	int n = sys3(SYS_read, sv[1], (int) (long) buf, sizeof buf);
	if (n != 4 || !eq(buf, "ping", 4)) { puts1("unixtest: socketpair recv FAIL\n"); ok = 0; }
	sys3(SYS_write, sv[1], (int) (long) "pong", 4);
	n = sys3(SYS_read, sv[0], (int) (long) buf, sizeof buf);
	if (n != 4 || !eq(buf, "pong", 4)) { puts1("unixtest: socketpair reverse FAIL\n"); ok = 0; }
	sys3(SYS_close, sv[0], 0, 0); sys3(SYS_close, sv[1], 0, 0);
	if (ok) puts1("unixtest: socketpair OK (ping/pong both ways)\n");

	/* 2) named stream: bind/listen/connect/accept on /tmp/unixtest.sock */
	a[0] = AF_UNIX; a[1] = SOCK_STREAM; a[2] = 0;
	int srv = sock(SC_SOCKET, a);
	unsigned char sa[110]; for (int i = 0; i < 110; i++) sa[i] = 0;
	const char* path = "/tmp/unixtest.sock";
	sa[0] = AF_UNIX;
	int pl = 0; while (path[pl]) { sa[2 + pl] = (unsigned char) path[pl]; pl++; }
	pl++;                                                /* include the trailing NUL */
	unsigned salen = 2 + pl;
	a[0] = srv; a[1] = (unsigned long) sa; a[2] = salen;
	if (sock(SC_BIND, a) < 0) { puts1("unixtest: bind FAIL\n"); return 1; }
	a[0] = srv; a[1] = 5;
	if (sock(SC_LISTEN, a) < 0) { puts1("unixtest: listen FAIL\n"); return 1; }
	a[0] = AF_UNIX; a[1] = SOCK_STREAM; a[2] = 0;
	int cli = sock(SC_SOCKET, a);
	a[0] = cli; a[1] = (unsigned long) sa; a[2] = salen;
	if (sock(SC_CONNECT, a) < 0) { puts1("unixtest: connect FAIL\n"); return 1; }
	a[0] = srv; a[1] = 0; a[2] = 0;
	int conn = sock(SC_ACCEPT, a);
	if (conn < 0) { puts1("unixtest: accept FAIL\n"); return 1; }
	sys3(SYS_write, cli, (int) (long) "hello", 5);
	n = sys3(SYS_read, conn, (int) (long) buf, sizeof buf);
	if (n == 5 && eq(buf, "hello", 5)) puts1("unixtest: named connect/accept OK (/tmp/unixtest.sock)\n");
	else { puts1("unixtest: named exchange FAIL\n"); ok = 0; }
	sys3(SYS_close, cli, 0, 0); sys3(SYS_close, conn, 0, 0); sys3(SYS_close, srv, 0, 0);

	puts1(ok ? "unixtest: ALL PASS\n" : "unixtest: SOME FAILED\n");
	return ok ? 0 : 1;
}
