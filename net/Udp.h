/*
 * Udp.h — UDP: port demux to sockets, datagram send with the pseudo-header checksum, and
 * ICMP port-unreachable for unbound ports. udpInit() registers udpRx as IP proto 17.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct NetBuf;
struct Socket;

enum { UDP_HLEN = 8 };

void udpInit();
void udpRx(NetBuf* skb);     // IP proto-17 handler: head() = UDP header, addrs set. Owns skb.

// Send `len` bytes from a UDP socket to dst. Builds the header (src port = socket's local port,
// auto-bound if needed) + pseudo-header checksum and hands it to ipOutput. Returns len or -errno.
int udpSend(Socket* s, const void* buf, unsigned len, uint32_t dstIp, uint16_t dstPort);

// Deliver an inbound ICMP error (already mapped to a positive errno) to the CONNECTED UDP
// socket whose 4-tuple matches the quoted datagram: (localIp, localPort) = the quote's source,
// (remoteIp, remotePort) = its destination. Unconnected sockets are ignored (Linux semantics
// without IP_RECVERR).
void udpIcmpError(uint32_t localIp, uint16_t localPort, uint32_t remoteIp, uint16_t remotePort, int err);

}  // namespace kernel
