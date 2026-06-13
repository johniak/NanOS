// FAZA E — host tests for the pure resolver parsers (user/libc-glue/resolv_parse.c): resolv.conf,
// /etc/services, /etc/protocols, the search/ndots candidate policy, PTR-name construction, and the
// DNS A/PTR answer decoders. No syscalls — data in, data out, exactly like the NxeLoader tests.
#include "doctest.h"
#include "resolv_parse.h"
#include <cstring>

TEST_CASE("resolv.conf: nameservers, search, domain, options") {
	struct resolv_conf c;
	const char* txt =
		"# comment\n"
		"nameserver 10.0.2.3\n"
		"nameserver 8.8.8.8\n"
		"nameserver 1.1.1.1\n"
		"nameserver 9.9.9.9\n"          // 4th — beyond MAXNS, ignored
		"search corp.example.com example.com\n"
		"options ndots:2 timeout:3 attempts:4\n";
	resolv_parse_conf(txt, &c);
	CHECK(c.nns == 3);                                  // capped at RESOLV_MAXNS
	CHECK(c.ns[0] == (10u | 0u<<8 | 2u<<16 | 3u<<24));   // network-order bytes [10,0,2,3]
	CHECK(c.ns[1] == (8u | 8u<<8 | 8u<<16 | 8u<<24));
	CHECK(c.nsearch == 2);
	CHECK(std::strcmp(c.search[0], "corp.example.com") == 0);
	CHECK(c.ndots == 2); CHECK(c.timeout == 3); CHECK(c.attempts == 4);
}

TEST_CASE("resolv.conf: defaults + lone domain directive") {
	struct resolv_conf c;
	resolv_parse_conf("domain lan\n", &c);
	CHECK(c.ndots == 1); CHECK(c.timeout == 5); CHECK(c.attempts == 2);  // defaults
	CHECK(c.nsearch == 1);
	CHECK(std::strcmp(c.search[0], "lan") == 0);
	CHECK(c.nns == 0);                                   // resolv.c then injects the slirp fallback
}

TEST_CASE("/etc/protocols: by name, by alias, by number, with tabs + comments") {
	const char* txt =
		"# Internet protocols\n"
		"ip\t0\tIP\t# internet protocol\n"
		"icmp\t1\tICMP\n"
		"tcp\t6\tTCP\n";
	char name[64]; int num;
	REQUIRE(resolv_find_protocol(txt, "tcp", 0, name, &num)); CHECK(num == 6);
	REQUIRE(resolv_find_protocol(txt, "ICMP", 0, name, &num)); CHECK(num == 1);  // alias match
	CHECK(std::strcmp(name, "icmp") == 0);                                       // canonical returned
	REQUIRE(resolv_find_protocol(txt, 0, 6, name, &num)); CHECK(std::strcmp(name, "tcp") == 0);
	CHECK(resolv_find_protocol(txt, "nope", 0, name, &num) == 0);
}

TEST_CASE("/etc/services: by name+proto, by port, alias, proto disambiguation") {
	const char* txt =
		"# services\n"
		"http\t\t80/tcp\t\twww\n"
		"domain\t\t53/udp\n"
		"domain\t\t53/tcp\n";
	char name[64], proto[16]; int port;
	REQUIRE(resolv_find_service(txt, "http", "tcp", 0, 0, name, &port, proto));
	CHECK(port == 80); CHECK(std::strcmp(proto, "tcp") == 0);
	REQUIRE(resolv_find_service(txt, "www", "tcp", 0, 0, name, &port, proto));   // alias
	CHECK(port == 80); CHECK(std::strcmp(name, "http") == 0);
	REQUIRE(resolv_find_service(txt, "domain", "tcp", 0, 0, name, &port, proto));
	CHECK(std::strcmp(proto, "tcp") == 0);                                       // proto picks the row
	REQUIRE(resolv_find_service(txt, 0, 0, 1, 80, name, &port, proto));          // by port
	CHECK(std::strcmp(name, "http") == 0);
	CHECK(resolv_find_service(txt, "nope", 0, 0, 0, name, &port, proto) == 0);
}

TEST_CASE("PTR query name is d.c.b.a.in-addr.arpa") {
	unsigned ip; unsigned char* b = (unsigned char*) &ip;
	b[0]=212; b[1]=77; b[2]=98; b[3]=9;            // 212.77.98.9 (network order bytes)
	char out[40];
	resolv_ptr_qname(ip, out);
	CHECK(std::strcmp(out, "9.98.77.212.in-addr.arpa") == 0);
}

TEST_CASE("search/ndots candidate ordering") {
	struct resolv_conf c;
	resolv_parse_conf("search a.com b.com\noptions ndots:1\n", &c);
	char out[RESOLV_MAXSEARCH + 1][RESOLV_MAXDNAME];

	// "host" has 0 dots (< ndots) -> search suffixes FIRST, literal last.
	int n = resolv_candidates("host", &c, out, RESOLV_MAXSEARCH + 1);
	REQUIRE(n == 3);
	CHECK(std::strcmp(out[0], "host.a.com") == 0);
	CHECK(std::strcmp(out[1], "host.b.com") == 0);
	CHECK(std::strcmp(out[2], "host") == 0);

	// "host.x" has 1 dot (>= ndots) -> literal FIRST, then suffixes.
	n = resolv_candidates("host.x", &c, out, RESOLV_MAXSEARCH + 1);
	REQUIRE(n == 3);
	CHECK(std::strcmp(out[0], "host.x") == 0);
	CHECK(std::strcmp(out[1], "host.x.a.com") == 0);

	// Trailing dot -> absolute, single candidate (dot stripped).
	n = resolv_candidates("host.com.", &c, out, RESOLV_MAXSEARCH + 1);
	REQUIRE(n == 1);
	CHECK(std::strcmp(out[0], "host.com") == 0);
}

TEST_CASE("DNS A answer decode (with a compression pointer)") {
	// header: id, flags, qd=1, an=1
	unsigned char m[] = {
		0x12,0x34, 0x81,0x80, 0,1, 0,1, 0,0, 0,0,
		// question: "wp.pl" A IN
		2,'w','p', 2,'p','l', 0, 0,1, 0,1,
		// answer: name ptr->0x0c, A, IN, ttl, rdlen 4, 212.77.98.9
		0xc0,0x0c, 0,1, 0,1, 0,0,0,60, 0,4, 212,77,98,9,
	};
	unsigned ip = 0;
	REQUIRE(resolv_dns_parse_a(m, (int) sizeof m, &ip));
	unsigned char* b = (unsigned char*) &ip;
	CHECK(b[0]==212); CHECK(b[1]==77); CHECK(b[2]==98); CHECK(b[3]==9);
}

TEST_CASE("DNS PTR answer decode") {
	unsigned char m[] = {
		0x12,0x34, 0x81,0x80, 0,1, 0,1, 0,0, 0,0,
		// question: "9" PTR IN (abbreviated qname, fine for the skip)
		1,'9', 0, 0,12, 0,1,
		// answer: name ptr->0x0c, PTR, IN, ttl, rdlen, "wp.pl"
		0xc0,0x0c, 0,12, 0,1, 0,0,0,60, 0,7, 2,'w','p', 2,'p','l', 0,
	};
	char name[64];
	REQUIRE(resolv_dns_parse_ptr(m, (int) sizeof m, name, sizeof name));
	CHECK(std::strcmp(name, "wp.pl") == 0);
}

TEST_CASE("dns_encode produces length-prefixed labels") {
	unsigned char q[32];
	int n = resolv_dns_encode(q, "wp.pl");
	REQUIRE(n == 7);
	CHECK(q[0]==2); CHECK(q[1]=='w'); CHECK(q[2]=='p');
	CHECK(q[3]==2); CHECK(q[4]=='p'); CHECK(q[5]=='l'); CHECK(q[6]==0);
}
