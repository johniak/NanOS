/*
 * resolv.c — the stub DNS resolver in libc.ndl (FAZA 11). getaddrinfo/gethostbyname read
 * /etc/hosts then query the nameserver from /etc/resolv.conf over UDP/53 (with DNS name
 * compression in the answer), exactly the path glibc's stub resolver takes — minus netlink/nscd
 * (which it only uses for optimisation and degrades without). IPv4-only: getaddrinfo returns
 * A/AF_INET records only, and AF_INET6 yields no addresses, the documented FAZA 11 semantics.
 */
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int h_errno;

/* Read the first `nameserver` from /etc/resolv.conf into ns (network-order). Returns 1 on
 * success, else 0 (caller falls back). */
static int read_nameserver(in_addr_t* ns) {
	int fd = open("/etc/resolv.conf", O_RDONLY);
	if (fd < 0) return 0;
	char buf[512];
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return 0;
	buf[n] = 0;
	for (char* p = buf; p && *p;) {
		if (strncmp(p, "nameserver", 10) == 0) {
			char* q = p + 10;
			while (*q == ' ' || *q == '\t') q++;
			char ipbuf[64]; int i = 0;
			while (i < 63 && *q && *q != '\n' && *q != ' ' && *q != '\t') ipbuf[i++] = *q++;
			ipbuf[i] = 0;
			struct in_addr a;
			if (inet_aton(ipbuf, &a)) { *ns = a.s_addr; return 1; }
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	return 0;
}

/* Look up `name` in /etc/hosts. Returns 1 + ip (network order) on a match. */
static int hosts_lookup(const char* name, in_addr_t* ip) {
	int fd = open("/etc/hosts", O_RDONLY);
	if (fd < 0) return 0;
	char buf[1024];
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return 0;
	buf[n] = 0;
	for (char* line = buf; line && *line;) {
		char* nl = strchr(line, '\n');
		if (nl) *nl = 0;
		if (*line && *line != '#') {
			char ipbuf[64], host[128];
			if (sscanf(line, "%63s %127s", ipbuf, host) == 2 && strcmp(host, name) == 0) {
				struct in_addr a;
				if (inet_aton(ipbuf, &a)) { *ip = a.s_addr; return 1; }
			}
		}
		if (!nl) break;
		line = nl + 1;
	}
	return 0;
}

/* Encode a hostname as DNS labels at q; returns the encoded length. */
static int dns_encode(unsigned char* q, const char* host) {
	int p = 0, lab = p++, n = 0;
	for (int i = 0;; i++) {
		char c = host[i];
		if (c == '.' || c == 0) { q[lab] = (unsigned char) n; if (c == 0) break; lab = p++; n = 0; }
		else { q[p++] = (unsigned char) c; n++; }
	}
	q[p++] = 0;
	return p;
}

/* Query the nameserver for an A record. Returns 1 + ip (network order) on success. */
static int dns_resolve(in_addr_t ns, const char* host, in_addr_t* ip) {
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return 0;
	struct sockaddr_in srv;
	memset(&srv, 0, sizeof(srv));
	srv.sin_family = AF_INET; srv.sin_port = htons(53); srv.sin_addr.s_addr = ns;
	if (connect(s, (struct sockaddr*) &srv, sizeof(srv)) < 0) { close(s); return 0; }

	unsigned char q[512];
	q[0] = 0x12; q[1] = 0x34; q[2] = 0x01; q[3] = 0x00;   /* id + RD */
	q[4] = 0; q[5] = 1; q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
	int ql = 12 + dns_encode(q + 12, host);
	q[ql++] = 0; q[ql++] = 1;     /* qtype A */
	q[ql++] = 0; q[ql++] = 1;     /* qclass IN */
	if (send(s, q, ql, 0) < 0) { close(s); return 0; }

	unsigned char r[1024];
	int n = recv(s, r, sizeof(r), 0);
	close(s);
	if (n < 12) return 0;
	int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
	int p = 12;
	for (int i = 0; i < qd; i++) {                        /* skip the question(s) */
		while (p < n && r[p]) { if ((r[p] & 0xc0) == 0xc0) { p += 2; goto qdone; } p += r[p] + 1; }
		p++;
	qdone:  p += 4;
	}
	for (int i = 0; i < an && p + 12 <= n; i++) {          /* scan answers for the first A */
		if ((r[p] & 0xc0) == 0xc0) p += 2;
		else { while (p < n && r[p]) p += r[p] + 1; p++; }
		int type = (r[p] << 8) | r[p + 1];
		int rdl = (r[p + 8] << 8) | r[p + 9];
		p += 10;
		if (type == 1 && rdl == 4 && p + 4 <= n) { memcpy(ip, r + p, 4); return 1; }
		p += rdl;
	}
	return 0;
}

/* Resolve `name` to an A address (network order): numeric -> /etc/hosts -> DNS. */
static int resolve(const char* name, in_addr_t* ip) {
	struct in_addr a;
	if (inet_aton(name, &a)) { *ip = a.s_addr; return 1; }   /* dotted-quad literal */
	if (hosts_lookup(name, ip)) return 1;
	in_addr_t ns;
	if (!read_nameserver(&ns)) ns = htonl(0x0a000203);       /* fallback: 10.0.2.3 (slirp DNS) */
	return dns_resolve(ns, name, ip);
}

static int parse_service(const char* service) {
	if (!service) return 0;
	if (service[0] >= '0' && service[0] <= '9') return atoi(service);
	if (!strcmp(service, "http")) return 80;
	if (!strcmp(service, "https")) return 443;
	if (!strcmp(service, "domain")) return 53;
	return 0;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
	if (!node && !service) return EAI_NONAME;
	int family = hints ? hints->ai_family : AF_UNSPEC;
	if (family == AF_INET6) return EAI_NODATA;               /* IPv4-only stack */
	int port = parse_service(service);

	in_addr_t ip;
	if (node) { if (!resolve(node, &ip)) return EAI_NONAME; }
	else      { ip = htonl((hints && (hints->ai_flags & AI_PASSIVE)) ? INADDR_ANY : INADDR_LOOPBACK); }

	struct addrinfo* ai = (struct addrinfo*) calloc(1, sizeof(struct addrinfo));
	struct sockaddr_in* sa = (struct sockaddr_in*) calloc(1, sizeof(struct sockaddr_in));
	if (!ai || !sa) { free(ai); free(sa); return EAI_MEMORY; }
	sa->sin_family = AF_INET; sa->sin_port = htons((uint16_t) port); sa->sin_addr.s_addr = ip;
	ai->ai_family = AF_INET;
	ai->ai_socktype = hints ? hints->ai_socktype : 0;
	ai->ai_protocol = hints ? hints->ai_protocol : 0;
	ai->ai_addrlen = sizeof(struct sockaddr_in);
	ai->ai_addr = (struct sockaddr*) sa;
	ai->ai_canonname = 0;
	ai->ai_next = 0;
	*res = ai;
	return 0;
}

void freeaddrinfo(struct addrinfo* res) {
	while (res) {
		struct addrinfo* nx = res->ai_next;
		free(res->ai_addr);
		free(res->ai_canonname);
		free(res);
		res = nx;
	}
}

const char* gai_strerror(int e) {
	switch (e) {
	case 0: return "Success";
	case EAI_NONAME: return "Name or service not known";
	case EAI_AGAIN: return "Temporary failure in name resolution";
	case EAI_FAIL: return "Non-recoverable failure in name resolution";
	case EAI_FAMILY: return "Address family not supported";
	case EAI_MEMORY: return "Memory allocation failure";
	case EAI_NODATA: return "No address associated with hostname";
	default: return "Unknown error";
	}
}

struct hostent* gethostbyname(const char* name) {
	static in_addr_t ip;
	static char* addr_list[2];
	static char namebuf[256];
	static struct hostent he;
	if (!resolve(name, &ip)) { h_errno = HOST_NOT_FOUND; return 0; }
	strncpy(namebuf, name, sizeof(namebuf) - 1); namebuf[sizeof(namebuf) - 1] = 0;
	addr_list[0] = (char*) &ip; addr_list[1] = 0;
	he.h_name = namebuf;
	static char* no_aliases[1] = { 0 };
	he.h_aliases = no_aliases;
	he.h_addrtype = AF_INET;
	he.h_length = 4;
	he.h_addr_list = addr_list;
	return &he;
}

int getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags) {
	(void) salen; (void) flags;
	const struct sockaddr_in* in = (const struct sockaddr_in*) sa;
	if (host && hostlen) inet_ntop(AF_INET, &in->sin_addr, host, hostlen);
	if (serv && servlen) snprintf(serv, servlen, "%u", ntohs(in->sin_port));
	return 0;
}
