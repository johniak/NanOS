/*
 * netinet/icmp6.h — ICMPv6 header + raw-socket filter (glibc i686 layout). NanOS is IPv4-only,
 * so ping is built --disable-ipv6 and never exercises these paths at runtime; the header exists
 * because ping's ping_common.h includes it unconditionally.
 */
#ifndef _NETINET_ICMP6_H
#define _NETINET_ICMP6_H

#include <stdint.h>
#include <string.h>
#include <netinet/in.h>

struct icmp6_hdr {
	uint8_t  icmp6_type;
	uint8_t  icmp6_code;
	uint16_t icmp6_cksum;
	union {
		uint32_t icmp6_un_data32[1];
		uint16_t icmp6_un_data16[2];
		uint8_t  icmp6_un_data8[4];
	} icmp6_dataun;
};
#define icmp6_data32    icmp6_dataun.icmp6_un_data32
#define icmp6_data16    icmp6_dataun.icmp6_un_data16
#define icmp6_data8     icmp6_dataun.icmp6_un_data8
#define icmp6_pptr      icmp6_data32[0]   /* parameter prob */
#define icmp6_mtu       icmp6_data32[0]   /* packet too big */
#define icmp6_id        icmp6_data16[0]   /* echo request/reply */
#define icmp6_seq       icmp6_data16[1]   /* echo request/reply */
#define icmp6_maxdelay  icmp6_data16[0]   /* mcast group membership */

/* ICMPv6 types. */
#define ICMP6_DST_UNREACH     1
#define ICMP6_PACKET_TOO_BIG  2
#define ICMP6_TIME_EXCEEDED   3
#define ICMP6_PARAM_PROB      4
#define ICMP6_ECHO_REQUEST    128
#define ICMP6_ECHO_REPLY      129

/* ICMP6_DST_UNREACH codes. */
#define ICMP6_DST_UNREACH_NOROUTE     0
#define ICMP6_DST_UNREACH_ADMIN       1
#define ICMP6_DST_UNREACH_BEYONDSCOPE 2
#define ICMP6_DST_UNREACH_ADDR        3
#define ICMP6_DST_UNREACH_NOPORT      4
#define ICMP6_DST_UNREACH_POLICYFAIL  5
#define ICMP6_DST_UNREACH_REJECTROUTE 6

/* ICMP6_TIME_EXCEEDED codes. */
#define ICMP6_TIME_EXCEED_TRANSIT     0
#define ICMP6_TIME_EXCEED_REASSEMBLY  1

/* ICMP6_PARAM_PROB codes. */
#define ICMP6_PARAMPROB_HEADER        0
#define ICMP6_PARAMPROB_NEXTHEADER    1
#define ICMP6_PARAMPROB_OPTION        2

/* Raw-socket ICMPv6 type filtering (RFC 3542). */
struct icmp6_filter {
	uint32_t icmp6_filt[8];
};
#define ICMP6_FILTER 1

#define ICMP6_FILTER_WILLPASS(type, filterp) \
	((((filterp)->icmp6_filt[(type) >> 5]) & (1U << ((type) & 31))) != 0)
#define ICMP6_FILTER_WILLBLOCK(type, filterp) \
	((((filterp)->icmp6_filt[(type) >> 5]) & (1U << ((type) & 31))) == 0)
#define ICMP6_FILTER_SETPASS(type, filterp) \
	((((filterp)->icmp6_filt[(type) >> 5]) |= (1U << ((type) & 31))))
#define ICMP6_FILTER_SETBLOCK(type, filterp) \
	((((filterp)->icmp6_filt[(type) >> 5]) &= ~(1U << ((type) & 31))))
#define ICMP6_FILTER_SETPASSALL(filterp) \
	memset(filterp, 0xFF, sizeof(struct icmp6_filter))
#define ICMP6_FILTER_SETBLOCKALL(filterp) \
	memset(filterp, 0, sizeof(struct icmp6_filter))

#endif /* _NETINET_ICMP6_H */
