/*
 * netinet/in.h — IPv4 (and a minimal IPv6 surface) address structures + constants, Linux i686
 * layout. sockaddr_in is 16 bytes: family (host order) + port + addr (both network order) + pad.
 */
#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <sys/socket.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr { in_addr_t s_addr; };

struct sockaddr_in {
	sa_family_t    sin_family;   /* AF_INET */
	in_port_t      sin_port;     /* network byte order */
	struct in_addr sin_addr;     /* network byte order */
	unsigned char  sin_zero[8];
};

struct in6_addr { unsigned char s6_addr[16]; };
struct sockaddr_in6 {
	sa_family_t     sin6_family;
	in_port_t       sin6_port;
	uint32_t        sin6_flowinfo;
	struct in6_addr sin6_addr;
	uint32_t        sin6_scope_id;
};
extern const struct in6_addr in6addr_any;

/* IPv6 address-test macros (sudo's address matching uses them). */
#ifndef IN6_IS_ADDR_UNSPECIFIED
#define IN6_IS_ADDR_UNSPECIFIED(a) \
	(((const uint32_t*)(a))[0]==0 && ((const uint32_t*)(a))[1]==0 && \
	 ((const uint32_t*)(a))[2]==0 && ((const uint32_t*)(a))[3]==0)
#define IN6_IS_ADDR_LOOPBACK(a) \
	(((const uint32_t*)(a))[0]==0 && ((const uint32_t*)(a))[1]==0 && \
	 ((const uint32_t*)(a))[2]==0 && ((const uint32_t*)(a))[3]==htonl(1))
#define IN6_ARE_ADDR_EQUAL(a,b) \
	(((const uint32_t*)(a))[0]==((const uint32_t*)(b))[0] && \
	 ((const uint32_t*)(a))[1]==((const uint32_t*)(b))[1] && \
	 ((const uint32_t*)(a))[2]==((const uint32_t*)(b))[2] && \
	 ((const uint32_t*)(a))[3]==((const uint32_t*)(b))[3])
/* IPv6 address-scope tests (c-ares getnameinfo, and general resolver code). The scope lives in the
 * first byte(s) of the address: fe80::/10 link-local, fec0::/10 site-local, ff00::/8 multicast (with
 * the low nibble of byte 1 selecting the multicast scope), ::ffff:0:0/96 v4-mapped. */
#define IN6_IS_ADDR_LINKLOCAL(a) \
	((((const unsigned char*)(a))[0]==0xfe) && ((((const unsigned char*)(a))[1]&0xc0)==0x80))
#define IN6_IS_ADDR_SITELOCAL(a) \
	((((const unsigned char*)(a))[0]==0xfe) && ((((const unsigned char*)(a))[1]&0xc0)==0xc0))
#define IN6_IS_ADDR_MULTICAST(a)     (((const unsigned char*)(a))[0]==0xff)
#define IN6_IS_ADDR_MC_LINKLOCAL(a)  (IN6_IS_ADDR_MULTICAST(a) && ((((const unsigned char*)(a))[1]&0x0f)==0x02))
#define IN6_IS_ADDR_V4MAPPED(a) \
	(((const uint32_t*)(a))[0]==0 && ((const uint32_t*)(a))[1]==0 && \
	 ((const uint32_t*)(a))[2]==htonl(0xffff))
#define IN6_IS_ADDR_V4COMPAT(a) \
	(((const uint32_t*)(a))[0]==0 && ((const uint32_t*)(a))[1]==0 && \
	 ((const uint32_t*)(a))[2]==0 && ((const uint32_t*)(a))[3]!=0 && \
	 ((const uint32_t*)(a))[3]!=htonl(1))
#endif

#define INADDR_ANY        ((in_addr_t) 0x00000000)
#define INADDR_BROADCAST  ((in_addr_t) 0xffffffff)
#define INADDR_NONE       ((in_addr_t) 0xffffffff)
#define INADDR_LOOPBACK   ((in_addr_t) 0x7f000001)
#define IN_LOOPBACKNET    127

/* Privileged-port boundary: ports below this are "reserved" (traceroute picks its source port
 * relative to it). */
#define IPPORT_RESERVED   1024

#define IPPROTO_IP    0
#define IPPROTO_ICMP  1
#define IPPROTO_IGMP  2
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17
#define IPPROTO_IPV6  41
#define IPPROTO_ICMPV6 58
#define IPPROTO_RAW   255

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

/* IP-level setsockopt (a couple apps probe these; benign no-ops in the kernel). */
#define IP_TOS      1
#define IP_TTL      2
#define IP_HDRINCL  3
#define IP_RECVERR  11
#define IP_MTU_DISCOVER 10

/* IPv4 multicast socket options + membership request structs (Linux values). libuv joins/leaves
 * multicast groups; NanOS treats the ioctls as benign no-ops but the source must compile. */
#define IP_MULTICAST_IF   32
#define IP_MULTICAST_TTL  33
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
#define IP_DROP_MEMBERSHIP 36
#define IP_UNBLOCK_SOURCE 37
#define IP_BLOCK_SOURCE   38
#define IP_ADD_SOURCE_MEMBERSHIP 39
#define IP_DROP_SOURCE_MEMBERSHIP 40
struct ip_mreq {
	struct in_addr imr_multiaddr;
	struct in_addr imr_interface;
};
struct ip_mreqn {
	struct in_addr imr_multiaddr;
	struct in_addr imr_address;
	int            imr_ifindex;
};
struct ip_mreq_source {
	struct in_addr imr_multiaddr;
	struct in_addr imr_interface;
	struct in_addr imr_sourceaddr;
};
struct ipv6_mreq {
	struct in6_addr ipv6mr_multiaddr;
	unsigned int    ipv6mr_interface;
};

/* IPv6-level setsockopt (Linux values). libuv probes these; benign no-ops in the kernel. */
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_ADD_MEMBERSHIP 20
#define IPV6_DROP_MEMBERSHIP 21
#define IPV6_V6ONLY         26
#define IPV6_RECVERR        25
#define IPV6_TCLASS         67

/* Byte order (host is little-endian i686): swap for the 'n' (network) forms. Defined here so
 * they're available wherever <netinet/in.h> is included, exactly as on Linux. */
#ifndef htons
#define htons(x) ((uint16_t) ((((uint16_t)(x) & 0xff) << 8) | (((uint16_t)(x) >> 8) & 0xff)))
#define ntohs(x) htons(x)
#define htonl(x) ((uint32_t) ((((uint32_t)(x) & 0xff) << 24) | (((uint32_t)(x) & 0xff00) << 8) | \
                              (((uint32_t)(x) >> 8) & 0xff00) | (((uint32_t)(x) >> 24) & 0xff)))
#define ntohl(x) htonl(x)
#endif

#ifdef __cplusplus
}
#endif

#endif /* _NETINET_IN_H */
