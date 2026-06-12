/*
 * Icmp.h — ICMPv4: echo request/reply (ping), destination-unreachable and time-exceeded
 * (the errors traceroute and UDP/TCP rely on). icmpInit() registers icmpRx as the IP handler
 * for protocol 1. Received ICMP is also delivered to a raw handler (SOCK_RAW, FAZA 7) so a
 * userland ping sees replies.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct NetBuf;

enum {
	ICMP_ECHO_REPLY   = 0,
	ICMP_DEST_UNREACH = 3,
	ICMP_ECHO_REQUEST = 8,
	ICMP_TIME_EXCEEDED = 11,
	ICMP_PARAM_PROBLEM = 12,
	// dest-unreachable codes
	ICMP_NET_UNREACH  = 0,
	ICMP_HOST_UNREACH = 1,
	ICMP_PROT_UNREACH = 2,
	ICMP_PORT_UNREACH = 3,
	ICMP_FRAG_NEEDED  = 4,
	// time-exceeded codes
	ICMP_TTL_EXCEEDED = 0,
	ICMP_FRAG_TIME_EXCEEDED = 1,
};

void icmpInit();
void icmpRx(NetBuf* skb);     // IP proto-1 handler: skb->head() = ICMP message, addrs set. Owns skb.

// Send an ICMP echo request to dst with the given id/seq and payload (host-side ping/keepalive).
// Returns ipOutput's result.
int icmpSendEcho(uint32_t dst, uint16_t id, uint16_t seq, const void* data, int len);

// Send an ICMP error (dest-unreachable / time-exceeded) about a received datagram. `orig` points
// at the offending IP header (in the received skb); we quote its header + first 8 bytes, per RFC.
void icmpSendError(NetBuf* origSkb, uint8_t type, uint8_t code);

// Raw delivery hook: SOCK_RAW/IPPROTO_ICMP sockets (FAZA 7) install this to receive ICMP. The
// handler is given the skb (head() at the ICMP message, saddr/daddr set) and owns it.
typedef void (*IcmpRawFn)(NetBuf* skb);
void icmpSetRawHandler(IcmpRawFn fn);

// Echo-reply hook (a kernel-side ping result; also used by the FAZA 6 verification probe).
typedef void (*IcmpEchoReplyFn)(uint32_t src, uint16_t id, uint16_t seq);
void icmpSetEchoReplyHandler(IcmpEchoReplyFn fn);

void icmpReset();   // tests

}  // namespace kernel
