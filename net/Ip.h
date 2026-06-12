/*
 * Ip.h — IPv4: header parse/build + checksum, routing-driven output (ARP next-hop resolution),
 * protocol demux (ICMP/UDP/TCP), and fragmentation/reassembly. ipInit() registers ipRx as the
 * Ethernet IP handler. All addresses host byte order.
 *
 * Reassembly note: a reassembled datagram is bounded by the 2 KiB NetBuf (our buffer
 * architecture). Target traffic (ICMP echo, DNS, TCP-MSS segments) never fragments, so this is
 * a buffer bound, not a logic shortcut; oversized reassemblies are dropped (and logged at the
 * reasm layer). Fragment ordering, MF/offset handling and timeout are fully implemented.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct NetBuf;
struct NetDevice;

enum {
	IPPROTO_ICMP = 1,
	IPPROTO_TCP  = 6,
	IPPROTO_UDP  = 17,
	IP_HLEN_MIN  = 20,
	IP_DEFAULT_TTL = 64,
	IP_FLAG_DF = 0x4000,     // don't fragment
	IP_FLAG_MF = 0x2000,     // more fragments
	IP_FRAG_MASK = 0x1FFF,   // fragment offset (in 8-byte units)
};

// A transport-protocol receive handler. On entry skb->head() is the transport header, and
// skb->saddr/daddr/ipproto are set. The handler owns the skb.
typedef void (*IpProtoHandler)(NetBuf* skb);

void ipInit();                                   // register ipRx with Ether
void ipSetHandler(uint8_t proto, IpProtoHandler h);

// L3 input (installed by ipInit). Validates, accepts-for-us, reassembles, demuxes. Owns skb.
void ipRx(NetBuf* skb);

// Send an IPv4 datagram: skb holds the transport payload at head(). Builds the header (src =
// egress device IP, given dst + proto), routes to a next hop, fragments if it exceeds the MTU,
// resolves the next-hop MAC via ARP (queuing if needed) and transmits. Consumes the skb.
// Returns 0 on success/queued, <0 on no-route. `df` sets the Don't-Fragment flag.
int ipOutput(uint32_t dst, uint8_t proto, NetBuf* skb, bool df = false);

// Reassembly aging (drop incomplete datagrams past the timeout). Called from the softirq tick.
void ipReasmTick(unsigned now);
void ipReasmSetClock(unsigned (*fn)());

void ipReset();   // tests: clear handlers + reassembly + the IP id counter

}  // namespace kernel
