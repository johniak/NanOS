/*
 * resolv.c — the stub DNS resolver in libc.ndl. getaddrinfo/gethostbyname read /etc/hosts then
 * query the nameservers from /etc/resolv.conf, honouring search domains + ndots, multiple
 * nameservers with timeout/attempts, and a TCP/53 retry when a UDP reply is truncated (TC). PTR
 * (reverse) lookups back gethostbyaddr/getnameinfo. /etc/services and /etc/protocols are read from
 * disk (built-in tables only as a fallback when the file is absent). This is the path glibc's stub
 * resolver takes — minus netlink/nscd. IPv4-only: A records only; AF_INET6 yields no addresses.
 *
 * All parsing/wire logic lives in the pure, host-tested resolv_parse.c; this file is just the
 * file + socket I/O around it.
 */
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "resolv_parse.h"

int h_errno;

/* Read a whole small text file into buf (NUL-terminated). Returns bytes read, or 0. */
static int read_file(const char* path, char* buf, int max) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	int n = read(fd, buf, max - 1);
	close(fd);
	if (n <= 0) return 0;
	buf[n] = 0;
	return n;
}

/* Load /etc/resolv.conf (defaults if absent); fall back to slirp's 10.0.2.3 if no nameserver. */
static void load_conf(struct resolv_conf* c) {
	char buf[1024];
	resolv_parse_conf(read_file("/etc/resolv.conf", buf, sizeof buf) ? buf : "", c);
	if (c->nns == 0) c->ns[c->nns++] = htonl(0x0a000203);
}

/* Look up `name` in /etc/hosts. Returns 1 + ip (network order) on a match. */
static int hosts_lookup(const char* name, in_addr_t* ip) {
	char buf[1024];
	if (!read_file("/etc/hosts", buf, sizeof buf)) return 0;
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

/* Receive with a timeout (seconds) via select(); returns bytes read or <0 on timeout/error. */
static int recv_timeout(int s, unsigned char* r, int rmax, int timeout_s) {
	fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
	struct timeval tv; tv.tv_sec = timeout_s; tv.tv_usec = 0;
	if (select(s + 1, &rf, 0, 0, &tv) <= 0) return -1;
	return recv(s, r, rmax, 0);
}

/* TCP/53 fallback (RFC 1035 §4.2.2): 2-byte length prefix + message, both directions. */
static int dns_tcp(in_addr_t ns, const unsigned char* q, int ql, unsigned char* r, int rmax, int timeout_s) {
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return -1;
	struct sockaddr_in srv; memset(&srv, 0, sizeof srv);
	srv.sin_family = AF_INET; srv.sin_port = htons(53); srv.sin_addr.s_addr = ns;
	if (connect(s, (struct sockaddr*) &srv, sizeof srv) < 0) { close(s); return -1; }
	unsigned char pfx[2] = { (unsigned char) (ql >> 8), (unsigned char) (ql & 0xff) };
	if (send(s, pfx, 2, 0) < 0 || send(s, q, ql, 0) < 0) { close(s); return -1; }
	unsigned char lenb[2];
	if (recv_timeout(s, lenb, 2, timeout_s) != 2) { close(s); return -1; }
	int want = (lenb[0] << 8) | lenb[1];
	if (want > rmax) want = rmax;
	int got = 0;
	while (got < want) {
		int k = recv_timeout(s, r + got, want - got, timeout_s);
		if (k <= 0) break;
		got += k;
	}
	close(s);
	return got;
}

/* Send `q` to one nameserver over UDP; on a truncated (TC) reply, retry over TCP. */
static int dns_one(in_addr_t ns, const unsigned char* q, int ql, unsigned char* r, int rmax, int timeout_s) {
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return -1;
	struct sockaddr_in srv; memset(&srv, 0, sizeof srv);
	srv.sin_family = AF_INET; srv.sin_port = htons(53); srv.sin_addr.s_addr = ns;
	if (connect(s, (struct sockaddr*) &srv, sizeof srv) < 0) { close(s); return -1; }
	if (send(s, q, ql, 0) < 0) { close(s); return -1; }
	int n = recv_timeout(s, r, rmax, timeout_s);
	close(s);
	if (n < 12) return -1;
	if (r[2] & 0x02) return dns_tcp(ns, q, ql, r, rmax, timeout_s);   /* TC bit: switch to TCP */
	return n;
}

/* Build a query for `qname` of the given qtype into q; returns its length. */
static int dns_build(unsigned char* q, const char* qname, int qtype) {
	q[0] = 0x12; q[1] = 0x34; q[2] = 0x01; q[3] = 0x00;   /* id + RD */
	q[4] = 0; q[5] = 1; q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;   /* qdcount 1 */
	int ql = 12 + resolv_dns_encode(q + 12, qname);
	q[ql++] = 0; q[ql++] = (unsigned char) qtype;
	q[ql++] = 0; q[ql++] = 1;                              /* qclass IN */
	return ql;
}

/* Run a query across the configured nameservers, `attempts` rounds, each with the conf timeout. */
static int dns_query(struct resolv_conf* c, const char* qname, int qtype, unsigned char* r, int rmax) {
	unsigned char q[512];
	int ql = dns_build(q, qname, qtype);
	for (int att = 0; att < c->attempts; att++)
		for (int i = 0; i < c->nns; i++) {
			int n = dns_one(c->ns[i], q, ql, r, rmax, c->timeout);
			if (n >= 12) return n;
		}
	return -1;
}

/* Resolve `name` to an A address (network order): numeric -> /etc/hosts -> DNS (search+ndots). */
static int resolve(const char* name, in_addr_t* ip) {
	struct in_addr a;
	if (inet_aton(name, &a)) { *ip = a.s_addr; return 1; }   /* dotted-quad literal */
	if (hosts_lookup(name, ip)) return 1;
	struct resolv_conf c; load_conf(&c);
	char cand[RESOLV_MAXSEARCH + 1][RESOLV_MAXDNAME];
	int nc = resolv_candidates(name, &c, cand, RESOLV_MAXSEARCH + 1);
	unsigned char r[1500];
	for (int i = 0; i < nc; i++) {
		int n = dns_query(&c, cand[i], 1 /*A*/, r, sizeof r);
		unsigned a32;
		if (n >= 12 && resolv_dns_parse_a(r, n, &a32)) { *ip = a32; return 1; }   /* in_addr_t is 'long' on i686 */
	}
	return 0;
}

/* PTR (reverse) lookup of a network-order IPv4 into out; returns 1 on success. */
static int ptr_lookup(in_addr_t ip, char* out, int len) {
	struct resolv_conf c; load_conf(&c);
	char qname[40];
	resolv_ptr_qname(ip, qname);
	unsigned char r[1500];
	int n = dns_query(&c, qname, 12 /*PTR*/, r, sizeof r);
	return n >= 12 && resolv_dns_parse_ptr(r, n, out, len);
}

static int parse_service(const char* service) {
	if (!service) return 0;
	if (service[0] >= '0' && service[0] <= '9') return atoi(service);
	struct servent* se = getservbyname(service, 0);
	return se ? ntohs((uint16_t) se->s_port) : 0;
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

/* gethostbyaddr — reverse (PTR) lookup. On no PTR record the caller falls back to numeric (like
 * `ping` without -n once it gets HOST_NOT_FOUND), so a missing reverse zone is not fatal. */
struct hostent* gethostbyaddr(const void* addr, socklen_t len, int type) {
	if (type != AF_INET || len < 4 || !addr) { h_errno = HOST_NOT_FOUND; return 0; }
	static in_addr_t ip;
	static char namebuf[256];
	static char* addr_list[2];
	static struct hostent he;
	ip = *(const in_addr_t*) addr;
	if (!ptr_lookup(ip, namebuf, sizeof namebuf)) { h_errno = HOST_NOT_FOUND; return 0; }
	addr_list[0] = (char*) &ip; addr_list[1] = 0;
	static char* no_aliases[1] = { 0 };
	he.h_name = namebuf; he.h_aliases = no_aliases;
	he.h_addrtype = AF_INET; he.h_length = 4; he.h_addr_list = addr_list;
	return &he;
}

/* Built-in /etc/protocols + /etc/services, used ONLY as a fallback when the on-disk file is
 * absent (the disk image ships both; the resolver reads them first). */
static const struct { const char* name; int proto; } g_protos[] = {
	{ "ip", 0 }, { "icmp", 1 }, { "igmp", 2 }, { "tcp", 6 },
	{ "udp", 17 }, { "ipv6", 41 }, { "ipv6-icmp", 58 }, { "icmpv6", 58 },
	{ "raw", 255 },
};
static struct protoent g_pe;
static char* g_pe_noaliases[1] = { 0 };
static char g_pe_name[64];

static struct protoent* fill_protoent(const char* name, int proto) {
	strncpy(g_pe_name, name, sizeof(g_pe_name) - 1); g_pe_name[sizeof(g_pe_name) - 1] = 0;
	g_pe.p_name = g_pe_name;
	g_pe.p_aliases = g_pe_noaliases;
	g_pe.p_proto = proto;
	return &g_pe;
}
struct protoent* getprotobyname(const char* name) {
	char buf[4096], cn[64]; int pn;
	if (read_file("/etc/protocols", buf, sizeof buf) && resolv_find_protocol(buf, name, 0, cn, &pn))
		return fill_protoent(cn, pn);
	for (unsigned i = 0; i < sizeof(g_protos) / sizeof(g_protos[0]); i++)   /* fallback table */
		if (strcmp(g_protos[i].name, name) == 0) return fill_protoent(g_protos[i].name, g_protos[i].proto);
	return 0;
}
struct protoent* getprotobynumber(int proto) {
	char buf[4096], cn[64]; int pn;
	if (read_file("/etc/protocols", buf, sizeof buf) && resolv_find_protocol(buf, 0, proto, cn, &pn))
		return fill_protoent(cn, pn);
	for (unsigned i = 0; i < sizeof(g_protos) / sizeof(g_protos[0]); i++)
		if (g_protos[i].proto == proto) return fill_protoent(g_protos[i].name, g_protos[i].proto);
	return 0;
}
struct protoent* getprotoent(void) { return 0; }
void setprotoent(int stayopen) { (void) stayopen; }
void endprotoent(void) {}

static const struct { const char* name; int port; const char* proto; } g_servs[] = {
	{ "echo", 7, "tcp" }, { "ftp", 21, "tcp" }, { "ssh", 22, "tcp" },
	{ "telnet", 23, "tcp" }, { "domain", 53, "udp" }, { "domain", 53, "tcp" },
	{ "http", 80, "tcp" }, { "https", 443, "tcp" }, { "tftp", 69, "udp" },
};
static struct servent g_se;
static char* g_se_noaliases[1] = { 0 };
static char g_se_name[64];
static char g_se_proto[16];
static struct servent* fill_servent(const char* name, int port, const char* proto) {
	strncpy(g_se_name, name, sizeof(g_se_name) - 1); g_se_name[sizeof(g_se_name) - 1] = 0;
	strncpy(g_se_proto, proto, sizeof(g_se_proto) - 1); g_se_proto[sizeof(g_se_proto) - 1] = 0;
	g_se.s_name = g_se_name;
	g_se.s_aliases = g_se_noaliases;
	g_se.s_port = htons((uint16_t) port);
	g_se.s_proto = g_se_proto;
	return &g_se;
}
struct servent* getservbyname(const char* name, const char* proto) {
	char buf[8192], cn[64], pr[16]; int port;
	if (read_file("/etc/services", buf, sizeof buf) &&
	    resolv_find_service(buf, name, proto, 0, 0, cn, &port, pr))
		return fill_servent(cn, port, pr);
	for (unsigned i = 0; i < sizeof(g_servs) / sizeof(g_servs[0]); i++)
		if (strcmp(g_servs[i].name, name) == 0 && (!proto || strcmp(g_servs[i].proto, proto) == 0))
			return fill_servent(g_servs[i].name, g_servs[i].port, g_servs[i].proto);
	return 0;
}
struct servent* getservbyport(int port, const char* proto) {
	int h = ntohs((uint16_t) port);
	char buf[8192], cn[64], pr[16]; int outp;
	if (read_file("/etc/services", buf, sizeof buf) &&
	    resolv_find_service(buf, 0, proto, 1, h, cn, &outp, pr))
		return fill_servent(cn, outp, pr);
	for (unsigned i = 0; i < sizeof(g_servs) / sizeof(g_servs[0]); i++)
		if (g_servs[i].port == h && (!proto || strcmp(g_servs[i].proto, proto) == 0))
			return fill_servent(g_servs[i].name, g_servs[i].port, g_servs[i].proto);
	return 0;
}

int getnameinfo(const struct sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags) {
	(void) salen;
	const struct sockaddr_in* in = (const struct sockaddr_in*) sa;
	if (host && hostlen) {
		/* Try a PTR lookup unless the caller demands numeric; fall back to numeric on no record. */
		if (!(flags & NI_NUMERICHOST) && ptr_lookup(in->sin_addr.s_addr, host, hostlen)) {
			/* got a name */
		} else {
			inet_ntop(AF_INET, &in->sin_addr, host, hostlen);
		}
	}
	if (serv && servlen) snprintf(serv, servlen, "%u", ntohs(in->sin_port));
	return 0;
}
