/*
 * resolv_parse.c — pure, syscall-free resolver parsers (see resolv_parse.h). Host-tested.
 */
#include "resolv_parse.h"
#include <string.h>
#include <stdlib.h>

/* Parse a dotted-quad into a network-order address (bytes laid out [a,b,c,d], like inet_aton). */
static int parse_ipv4(const char* s, unsigned* out) {
	unsigned b[4] = {0,0,0,0}; int bi = 0, digits = 0; unsigned v = 0;
	for (const char* p = s;; p++) {
		if (*p >= '0' && *p <= '9') {
			v = v * 10 + (unsigned)(*p - '0'); digits++;
			if (v > 255) return 0;
		}
		else if (*p == '.') { if (!digits || bi >= 3) return 0; b[bi++] = v; v = 0; digits = 0; }
		else if (*p == 0 || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
			if (!digits || bi != 3) return 0; b[3] = v; break;
		} else return 0;
	}
	*out = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
	return 1;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Copy the current line (up to '\n' or end) into `line` (truncating), strip a trailing comment.
 * Advances *pp past the newline. Returns 1 while there is a line, 0 at end. */
static int next_line(const char** pp, char* line, int max) {
	const char* p = *pp;
	if (!*p) return 0;
	int i = 0;
	while (*p && *p != '\n') { if (i < max - 1) line[i++] = *p; p++; }
	if (*p == '\n') p++;
	line[i] = 0;
	for (int j = 0; line[j]; j++) if (line[j] == '#' || line[j] == ';') { line[j] = 0; break; }
	*pp = p;
	return 1;
}

/* Read the next whitespace-delimited token from *pp into out; returns its length (0 at end). */
static int next_token(const char** pp, char* out, int max) {
	const char* p = *pp;
	while (is_space(*p)) p++;
	int i = 0;
	while (*p && !is_space(*p)) { if (i < max - 1) out[i++] = *p; p++; }
	out[i] = 0;
	*pp = p;
	return i;
}

void resolv_parse_conf(const char* buf, struct resolv_conf* c) {
	c->nns = 0; c->nsearch = 0; c->ndots = 1; c->timeout = 5; c->attempts = 2;
	if (!buf) return;
	const char* p = buf; char line[RESOLV_MAXDNAME * 2];
	while (next_line(&p, line, sizeof line)) {
		const char* lp = line; char kw[32];
		if (!next_token(&lp, kw, sizeof kw)) continue;
		if (!strcmp(kw, "nameserver")) {
			char ip[64]; unsigned a;
			if (next_token(&lp, ip, sizeof ip) && c->nns < RESOLV_MAXNS && parse_ipv4(ip, &a))
				c->ns[c->nns++] = a;
		} else if (!strcmp(kw, "search")) {
			c->nsearch = 0; char d[RESOLV_MAXDNAME];
			while (next_token(&lp, d, sizeof d) && c->nsearch < RESOLV_MAXSEARCH) {
				strncpy(c->search[c->nsearch], d, RESOLV_MAXDNAME - 1);
				c->search[c->nsearch][RESOLV_MAXDNAME - 1] = 0; c->nsearch++;
			}
		} else if (!strcmp(kw, "domain")) {
			char d[RESOLV_MAXDNAME];
			if (c->nsearch == 0 && next_token(&lp, d, sizeof d)) {
				strncpy(c->search[0], d, RESOLV_MAXDNAME - 1);
				c->search[0][RESOLV_MAXDNAME - 1] = 0; c->nsearch = 1;
			}
		} else if (!strcmp(kw, "options")) {
			char o[64];
			while (next_token(&lp, o, sizeof o)) {
				if (!strncmp(o, "ndots:", 6))         c->ndots    = atoi(o + 6);
				else if (!strncmp(o, "timeout:", 8))  c->timeout  = atoi(o + 8);
				else if (!strncmp(o, "attempts:", 9)) c->attempts = atoi(o + 9);
			}
		}
	}
}

int resolv_find_protocol(const char* buf, const char* name, int num, char* nameOut, int* protoOut) {
	if (!buf) return 0;
	const char* p = buf; char line[256];
	while (next_line(&p, line, sizeof line)) {
		const char* lp = line; char canon[64], numtok[16];
		if (!next_token(&lp, canon, sizeof canon)) continue;
		if (!next_token(&lp, numtok, sizeof numtok)) continue;
		int pn = atoi(numtok);
		int match = 0;
		if (name) {
			if (!strcmp(canon, name)) match = 1;
			else { char al[64]; const char* save = lp;
				while (next_token(&lp, al, sizeof al)) if (!strcmp(al, name)) { match = 1; break; }
				lp = save; }
		} else match = (pn == num);
		if (match) { strncpy(nameOut, canon, 63); nameOut[63] = 0; *protoOut = pn; return 1; }
	}
	return 0;
}

int resolv_find_service(const char* buf, const char* name, const char* proto, int byPort, int port,
                        char* nameOut, int* portOut, char* protoOut) {
	if (!buf) return 0;
	const char* p = buf; char line[256];
	while (next_line(&p, line, sizeof line)) {
		const char* lp = line; char canon[64], pp[32];
		if (!next_token(&lp, canon, sizeof canon)) continue;
		if (!next_token(&lp, pp, sizeof pp)) continue;        /* "port/proto" */
		char* slash = strchr(pp, '/');
		if (!slash) continue;
		*slash = 0; int sport = atoi(pp); const char* sproto = slash + 1;
		if (proto && strcmp(sproto, proto) != 0) continue;
		int match = 0;
		if (byPort) match = (sport == port);
		else if (!strcmp(canon, name)) match = 1;
		else { char al[64]; const char* save = lp;
			while (next_token(&lp, al, sizeof al)) if (!strcmp(al, name)) { match = 1; break; }
			lp = save; }
		if (match) {
			strncpy(nameOut, canon, 63); nameOut[63] = 0;
			strncpy(protoOut, sproto, 15); protoOut[15] = 0;
			*portOut = sport; return 1;
		}
	}
	return 0;
}

int resolv_ptr_qname(unsigned ip_net, char* out) {
	const unsigned char* b = (const unsigned char*) &ip_net;      /* network order: [a,b,c,d] */
	int n = 0;
	for (int i = 3; i >= 0; i--) {
		unsigned v = b[i];
		if (v >= 100) out[n++] = '0' + (v / 100);
		if (v >= 10)  out[n++] = '0' + (v / 10) % 10;
		out[n++] = '0' + v % 10;
		out[n++] = '.';
	}
	const char* suf = "in-addr.arpa";
	for (int i = 0; suf[i]; i++) out[n++] = suf[i];
	out[n] = 0;
	return n;
}

int resolv_dns_encode(unsigned char* q, const char* host) {
	int p = 0, lab = p++, n = 0;
	for (int i = 0;; i++) {
		char c = host[i];
		if (c == '.' || c == 0) { q[lab] = (unsigned char) n; if (c == 0) break; lab = p++; n = 0; }
		else { q[p++] = (unsigned char) c; n++; }
	}
	q[p++] = 0;
	return p;
}

/* Skip a DNS name at offset p (handles compression pointers); returns the offset just past it. */
static int dns_skip_name(const unsigned char* r, int n, int p) {
	while (p < n && r[p]) {
		if ((r[p] & 0xc0) == 0xc0) return p + 2;
		p += r[p] + 1;
	}
	return p + 1;
}

int resolv_dns_parse_a(const unsigned char* r, int n, unsigned* ip) {
	if (n < 12) return 0;
	int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
	int p = 12;
	for (int i = 0; i < qd; i++) p = dns_skip_name(r, n, p) + 4;   /* skip QNAME + qtype/qclass */
	for (int i = 0; i < an && p + 10 <= n; i++) {
		p = dns_skip_name(r, n, p);
		if (p + 10 > n) break;
		int type = (r[p] << 8) | r[p + 1];
		int rdl = (r[p + 8] << 8) | r[p + 9];
		p += 10;
		if (type == 1 && rdl == 4 && p + 4 <= n) { memcpy(ip, r + p, 4); return 1; }
		p += rdl;
	}
	return 0;
}

/* Decode a (possibly compressed) DNS name at offset p into out; returns 1 on success. */
static int dns_decode_name(const unsigned char* r, int n, int p, char* out, int len) {
	int o = 0, hops = 0;
	while (p < n && r[p]) {
		if ((r[p] & 0xc0) == 0xc0) {
			if (p + 1 >= n || ++hops > 16) return 0;
			p = ((r[p] & 0x3f) << 8) | r[p + 1];
			continue;
		}
		int l = r[p++];
		if (p + l > n) return 0;
		for (int i = 0; i < l; i++) { if (o < len - 1) out[o++] = r[p + i]; }
		p += l;
		if (r[p]) { if (o < len - 1) out[o++] = '.'; }
	}
	out[o] = 0;
	return o > 0;
}

int resolv_dns_parse_ptr(const unsigned char* r, int n, char* nameOut, int len) {
	if (n < 12) return 0;
	int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
	int p = 12;
	for (int i = 0; i < qd; i++) p = dns_skip_name(r, n, p) + 4;
	for (int i = 0; i < an && p + 10 <= n; i++) {
		p = dns_skip_name(r, n, p);
		if (p + 10 > n) break;
		int type = (r[p] << 8) | r[p + 1];
		int rdl = (r[p + 8] << 8) | r[p + 9];
		p += 10;
		if (type == 12 && p + rdl <= n) return dns_decode_name(r, n, p, nameOut, len);
		p += rdl;
	}
	return 0;
}

int resolv_candidates(const char* name, const struct resolv_conf* c,
                      char out[][RESOLV_MAXDNAME], int max) {
	int n = 0, ln = (int) strlen(name);
	/* A trailing dot means fully-qualified: try it verbatim (minus the dot) and nothing else. */
	if (ln > 0 && name[ln - 1] == '.') {
		if (n < max) { int k = ln - 1 < RESOLV_MAXDNAME - 1 ? ln - 1 : RESOLV_MAXDNAME - 1;
			memcpy(out[n], name, k); out[n][k] = 0; n++; }
		return n;
	}
	int dots = 0; for (int i = 0; i < ln; i++) if (name[i] == '.') dots++;
	#define ADD_LITERAL() do { if (n < max) { strncpy(out[n], name, RESOLV_MAXDNAME - 1); \
		out[n][RESOLV_MAXDNAME - 1] = 0; n++; } } while (0)
	#define ADD_SEARCH() do { for (int i = 0; i < c->nsearch && n < max; i++) { \
		int w = 0; for (int j = 0; name[j] && w < RESOLV_MAXDNAME - 1; j++) out[n][w++] = name[j]; \
		if (w < RESOLV_MAXDNAME - 1) out[n][w++] = '.'; \
		for (int j = 0; c->search[i][j] && w < RESOLV_MAXDNAME - 1; j++) out[n][w++] = c->search[i][j]; \
		out[n][w] = 0; n++; } } while (0)
	if (dots >= c->ndots) { ADD_LITERAL(); ADD_SEARCH(); }   /* enough dots: absolute first */
	else { ADD_SEARCH(); ADD_LITERAL(); }                    /* short name: search list first */
	#undef ADD_LITERAL
	#undef ADD_SEARCH
	return n;
}
