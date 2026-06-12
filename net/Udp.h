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

}  // namespace kernel
