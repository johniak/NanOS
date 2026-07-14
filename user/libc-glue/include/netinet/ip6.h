/*
 * netinet/ip6.h — the IPv6 header layout (RFC 8200). Node's tcp_wrap.cc / socket code includes it.
 * NanOS's kernel network stack doesn't parse IPv6 yet, but the struct is standard so the source
 * compiles and any userland IPv6 header handling is byte-correct.
 */
#ifndef _NETINET_IP6_H
#define _NETINET_IP6_H

#include <stdint.h>
#include <netinet/in.h>

struct ip6_hdr {
	union {
		struct ip6_hdrctl {
			uint32_t ip6_un1_flow;   /* version(4) traffic-class(8) flow-label(20) */
			uint16_t ip6_un1_plen;   /* payload length */
			uint8_t  ip6_un1_nxt;    /* next header */
			uint8_t  ip6_un1_hlim;   /* hop limit */
		} ip6_un1;
		uint8_t ip6_un2_vfc;         /* version(4) + traffic-class high nibble */
	} ip6_ctlun;
	struct in6_addr ip6_src;
	struct in6_addr ip6_dst;
};

#define ip6_vfc  ip6_ctlun.ip6_un2_vfc
#define ip6_flow ip6_ctlun.ip6_un1.ip6_un1_flow
#define ip6_plen ip6_ctlun.ip6_un1.ip6_un1_plen
#define ip6_nxt  ip6_ctlun.ip6_un1.ip6_un1_nxt
#define ip6_hlim ip6_ctlun.ip6_un1.ip6_un1_hlim
#define ip6_hops ip6_ctlun.ip6_un1.ip6_un1_hlim

#endif /* _NETINET_IP6_H */
