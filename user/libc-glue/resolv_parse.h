/*
 * resolv_parse.h — the PURE (syscall-free) core of the libc stub resolver: parsers for
 * /etc/resolv.conf, /etc/services, /etc/protocols, the DNS answer wire format, the search/ndots
 * candidate-name algorithm, and PTR query-name construction. Kept separate from resolv.c (which
 * does the file/socket I/O) so this logic is host-tested in tests/test_resolv.cpp, exactly like
 * NxeLoader — no #ifdef, no mocks, just data in / data out.
 *
 * IPs are 32-bit network order. No libc <netdb.h> types leak in; resolv.c maps the plain outputs
 * onto struct servent/protoent/hostent.
 */
#ifndef RESOLV_PARSE_H
#define RESOLV_PARSE_H

#ifdef __cplusplus
extern "C" {
#endif

#define RESOLV_MAXNS     3
#define RESOLV_MAXSEARCH 6
#define RESOLV_MAXDNAME  256

struct resolv_conf {
	unsigned ns[RESOLV_MAXNS]; int nns;             /* nameserver addresses, network order */
	char     search[RESOLV_MAXSEARCH][RESOLV_MAXDNAME]; int nsearch;
	int      ndots, timeout, attempts;              /* options ndots:/timeout:/attempts: */
};

/* Parse a resolv.conf text buffer. Always fills defaults (ndots 1, timeout 5, attempts 2). */
void resolv_parse_conf(const char* buf, struct resolv_conf* c);

/* /etc/protocols lookup. byName: match the official name OR an alias. Else match the number.
 * On success copies the canonical name into nameOut (>=64 B), sets *protoOut, returns 1. */
int resolv_find_protocol(const char* buf, const char* name, int num, char* nameOut, int* protoOut);

/* /etc/services lookup. byPort==0: match name|alias (+ optional proto). byPort!=0: match port
 * (host order) + optional proto. Copies canonical name (>=64 B) + proto (>=16 B), sets *portOut. */
int resolv_find_service(const char* buf, const char* name, const char* proto, int byPort, int port,
                        char* nameOut, int* portOut, char* protoOut);

/* Build the reverse-lookup name "d.c.b.a.in-addr.arpa" for a network-order IPv4 address. */
int resolv_ptr_qname(unsigned ip_net, char* out /* >= 32 B */);

/* DNS wire helpers. encode: write `host` as length-prefixed labels at q, return bytes written.
 * parse_a: scan a response for the first A record. parse_ptr: scan for the first PTR name. */
int resolv_dns_encode(unsigned char* q, const char* host);
int resolv_dns_parse_a(const unsigned char* r, int n, unsigned* ip);
int resolv_dns_parse_ptr(const unsigned char* r, int n, char* nameOut, int len);

/* Apply the glibc/musl search+ndots policy: produce the ordered list of FQDN candidates to try
 * for `name`. Returns the count written to out (each entry < RESOLV_MAXDNAME). */
int resolv_candidates(const char* name, const struct resolv_conf* c,
                      char out[][RESOLV_MAXDNAME], int max);

#ifdef __cplusplus
}
#endif

#endif /* RESOLV_PARSE_H */
