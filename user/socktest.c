/*
 * socktest — a ring-3 smoke test for the FAZA 9 socket syscall ABI. Self-contained (raw int
 * 0x80, no libc), it TCP-connects to 1.1.1.1:80 via socketcall(2), sends an HTTP/1.0 request and
 * prints the reply's first line. Proves the userland -> int 0x80 -> socketcall -> TCP -> wire
 * path end to end (and that the net call chain fits a process's kernel syscall stack).
 */
static inline int sys3(int nr, int a, int b, int c) {
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}
#define SYS_socketcall 102
#define SYS_write 4
#define SYS_exit 1

/* socketcall sub-calls */
#define SC_SOCKET 1
#define SC_CONNECT 3
#define SC_SEND 9
#define SC_RECV 10

static int sock(int call, unsigned long* args) { return sys3(SYS_socketcall, call, (int) (long) args, 0); }
static void puts1(const char* s) { int n = 0; while (s[n]) n++; sys3(SYS_write, 1, (int) (long) s, n); }

int main(void) {
	unsigned long a[6];
	a[0] = 2; a[1] = 1; a[2] = 0;                 /* socket(AF_INET, SOCK_STREAM, 0) */
	int fd = sock(SC_SOCKET, a);
	if (fd < 0) { puts1("socktest: socket() failed\n"); return 1; }

	unsigned char sa[16];
	for (int i = 0; i < 16; i++) sa[i] = 0;
	sa[0] = 2;                                    /* AF_INET */
	sa[2] = 0; sa[3] = 80;                        /* port 80, network order */
	sa[4] = 1; sa[5] = 1; sa[6] = 1; sa[7] = 1;   /* 1.1.1.1 */
	a[0] = fd; a[1] = (unsigned long) sa; a[2] = 16;
	if (sock(SC_CONNECT, a) < 0) { puts1("socktest: connect() failed\n"); return 1; }
	puts1("socktest: connected to 1.1.1.1:80\n");

	const char* req = "GET / HTTP/1.0\r\nHost: one.one.one.one\r\n\r\n";
	int rl = 0; while (req[rl]) rl++;
	a[0] = fd; a[1] = (unsigned long) req; a[2] = rl; a[3] = 0;
	sock(SC_SEND, a);

	char buf[256];
	a[0] = fd; a[1] = (unsigned long) buf; a[2] = 255; a[3] = 0;
	int n = sock(SC_RECV, a);
	if (n > 0) { puts1("socktest reply: "); sys3(SYS_write, 1, (int) (long) buf, n); }
	else puts1("socktest: no reply\n");
	return 0;
}
