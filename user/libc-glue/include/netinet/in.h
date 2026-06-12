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

#define INADDR_ANY        ((in_addr_t) 0x00000000)
#define INADDR_BROADCAST  ((in_addr_t) 0xffffffff)
#define INADDR_NONE       ((in_addr_t) 0xffffffff)
#define INADDR_LOOPBACK   ((in_addr_t) 0x7f000001)
#define IN_LOOPBACKNET    127

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
