/*
 * pingtest — proves the `ping wp.pl` MECHANISM end to end from ring 3, self-contained (raw int
 * 0x80 socketcall, no libc): resolve wp.pl via DNS (UDP/53 to the slirp forwarder 10.0.2.3),
 * then ICMP-echo the resolved address over a RAW socket and report the reply. This exercises the
 * exact path the GNU inetutils ping uses (getaddrinfo + SOCK_RAW/ICMP); the SDK port of the real
 * binary (FAZA 13) is then just packaging the same syscalls behind libc.
 */
static inline int sys3(int nr, int a, int b, int c) {
	int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory"); return r;
}
#define SYS_socketcall 102
#define SYS_write 4
#define SC_SOCKET 1
#define SC_CONNECT 3
#define SC_SENDTO 11
#define SC_RECVFROM 12
#define SC_SEND 9
#define SC_RECV 10

static int sock(int call, unsigned long* a) { return sys3(SYS_socketcall, call, (int) (long) a, 0); }
static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void puts1(const char* s) { sys3(SYS_write, 1, (int) (long) s, slen(s)); }
static void putdec(unsigned v) { char b[12]; int i = 11; b[i--] = 0; if (!v) b[i--] = '0'; while (v) { b[i--] = '0' + v % 10; v /= 10; } sys3(SYS_write, 1, (int) (long) (b + i + 1), slen(b + i + 1)); }
static void putip(unsigned char* ip) { for (int i = 0; i < 4; i++) { putdec(ip[i]); if (i < 3) puts1("."); } }

/* sockaddr_in (16 bytes): family LE, port BE, addr BE. */
static void mkaddr(unsigned char* sa, unsigned char a, unsigned char b, unsigned char c, unsigned char d, int port) {
	for (int i = 0; i < 16; i++) sa[i] = 0;
	sa[0] = 2; sa[2] = (port >> 8) & 0xff; sa[3] = port & 0xff;
	sa[4] = a; sa[5] = b; sa[6] = c; sa[7] = d;
}

/* Build a DNS A-record query for `host` into q; returns its length. */
static int dns_query(unsigned char* q, const char* host) {
	q[0] = 0x12; q[1] = 0x34;          /* id */
	q[2] = 0x01; q[3] = 0x00;          /* flags: RD */
	q[4] = 0; q[5] = 1;                /* qdcount = 1 */
	q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
	int p = 12, lab = p++;
	int n = 0;
	for (int i = 0;; i++) {
		char ch = host[i];
		if (ch == '.' || ch == 0) { q[lab] = (unsigned char) n; if (ch == 0) break; lab = p++; n = 0; }
		else { q[p++] = (unsigned char) ch; n++; }
	}
	q[p++] = 0;                        /* root label */
	q[p++] = 0; q[p++] = 1;            /* qtype A */
	q[p++] = 0; q[p++] = 1;            /* qclass IN */
	return p;
}

/* Parse the first A record out of a DNS response; fills ip[4], returns 0 on success. */
static int dns_parse(unsigned char* r, int len, unsigned char* ip) {
	if (len < 12) return -1;
	int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
	int p = 12;
	for (int i = 0; i < qd; i++) {           /* skip questions */
		while (p < len && r[p]) { if ((r[p] & 0xc0) == 0xc0) { p += 2; goto qdone; } p += r[p] + 1; }
		p++;
	qdone:  p += 4;                          /* qtype + qclass */
	}
	for (int i = 0; i < an && p + 12 <= len; i++) {
		if ((r[p] & 0xc0) == 0xc0) p += 2;   /* compressed name pointer */
		else { while (p < len && r[p]) p += r[p] + 1; p++; }
		int type = (r[p] << 8) | r[p + 1];
		int rdl = (r[p + 8] << 8) | r[p + 9];
		p += 10;
		if (type == 1 && rdl == 4) { for (int k = 0; k < 4; k++) ip[k] = r[p + k]; return 0; }
		p += rdl;
	}
	return -1;
}

static unsigned short inet_csum(unsigned char* d, int n) {
	unsigned long s = 0;
	for (int i = 0; i + 1 < n; i += 2) s += (d[i] << 8) | d[i + 1];
	if (n & 1) s += d[n - 1] << 8;
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return (unsigned short) ~s;
}

int main(void) {
	unsigned long a[6];
	unsigned char sa[16], buf[512];

	/* ---- DNS: resolve wp.pl via the slirp forwarder 10.0.2.3:53 ---- */
	a[0] = 2; a[1] = 2; a[2] = 0;                          /* socket(AF_INET, SOCK_DGRAM, 0) */
	int us = sock(SC_SOCKET, a);
	mkaddr(sa, 10, 0, 2, 3, 53);
	a[0] = us; a[1] = (unsigned long) sa; a[2] = 16; sock(SC_CONNECT, a);
	int qn = dns_query(buf, "wp.pl");
	a[0] = us; a[1] = (unsigned long) buf; a[2] = qn; a[3] = 0; sock(SC_SEND, a);
	a[0] = us; a[1] = (unsigned long) buf; a[2] = sizeof(buf); a[3] = 0;
	int rn = sock(SC_RECV, a);
	unsigned char ip[4];
	if (rn <= 0 || dns_parse(buf, rn, ip) != 0) { puts1("pingtest: DNS resolve of wp.pl FAILED\n"); return 1; }
	puts1("pingtest: wp.pl resolved to "); putip(ip); puts1("\n");

	/* ---- ICMP: echo the resolved address over a RAW socket ---- */
	a[0] = 2; a[1] = 3; a[2] = 1;                          /* socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) */
	int rs = sock(SC_SOCKET, a);
	if (rs < 0) { puts1("pingtest: RAW socket FAILED\n"); return 1; }
	unsigned char icmp[64];
	for (int i = 0; i < 64; i++) icmp[i] = 0;
	icmp[0] = 8;                                           /* echo request */
	icmp[4] = 0x12; icmp[5] = 0x34;                        /* id */
	icmp[6] = 0; icmp[7] = 1;                              /* seq */
	for (int i = 8; i < 64; i++) icmp[i] = (unsigned char) i;
	unsigned short c = inet_csum(icmp, 64);
	icmp[2] = (c >> 8) & 0xff; icmp[3] = c & 0xff;
	mkaddr(sa, ip[0], ip[1], ip[2], ip[3], 0);
	a[0] = rs; a[1] = (unsigned long) icmp; a[2] = 64; a[3] = 0; a[4] = (unsigned long) sa; a[5] = 16;
	sock(SC_SENDTO, a);
	a[0] = rs; a[1] = (unsigned long) buf; a[2] = sizeof(buf); a[3] = 0; a[4] = 0; a[5] = 0;
	int n = sock(SC_RECVFROM, a);
	if (n > 0) {
		int ihl = (buf[0] & 0x0f) * 4;                     /* RAW delivers the IP header */
		if (n >= ihl + 8 && buf[ihl] == 0) {               /* ICMP echo reply */
			puts1("pingtest: reply from "); putip(buf + 12); puts1("  <-- ping wp.pl OK (DNS + ICMP)\n");
		} else { puts1("pingtest: got a packet but not an echo reply\n"); }
	} else {
		puts1("pingtest: no ICMP reply (slirp may not forward ICMP to the internet)\n");
	}
	return 0;
}
