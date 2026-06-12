#include "Udp.h"
#include "Socket.h"
#include "Ip.h"
#include "Icmp.h"
#include "Ether.h"     // NET_HEADROOM
#include "Route.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include "NetStats.h"   // /proc/net/snmp counters
#include <string.h>

namespace kernel {

void udpInit() { ipSetHandler(IPPROTO_UDP, udpRx); }

void udpRx(NetBuf* skb) {
	if (!skb) return;
	if (skb->len < UDP_HLEN) { netbufFree(skb); return; }
	const unsigned char* u = skb->head();
	uint16_t srcPort = rd16be(u + 0);
	uint16_t dstPort = rd16be(u + 2);
	uint16_t ulen    = rd16be(u + 4);
	uint16_t csum    = rd16be(u + 6);
	if (ulen < UDP_HLEN || ulen > skb->len) { netbufFree(skb); return; }
	skb->trim(ulen);
	// Verify the checksum if present (0 = sender omitted it, allowed for IPv4 UDP).
	if (csum != 0 && inetPseudoChecksum(skb->saddr, skb->daddr, IPPROTO_UDP, u, ulen) != 0) {
		netbufFree(skb);
		return;
	}
	Socket* s = socketLookupUdp(skb->daddr, dstPort, skb->saddr, srcPort);
	if (!s) {
		// No listener: ICMP port-unreachable (unless it was a broadcast/multicast). l3/l4 still set.
		g_netStats.udpNoPorts++;
		if (skb->daddr != 0xFFFFFFFFu)
			icmpSendError(skb, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH);
		netbufFree(skb);
		return;
	}
	g_netStats.udpInDatagrams++;
	skb->pull(UDP_HLEN);                       // hand the payload to the socket
	if (!socketDeliver(s, skb, skb->saddr, srcPort))
		netbufFree(skb);                       // socket buffer full: drop (UDP has no flow control)
}

void udpIcmpError(uint32_t localIp, uint16_t localPort, uint32_t remoteIp, uint16_t remotePort, int err) {
	// socketLookupUdp(dstIp, dstPort, srcIp, srcPort) matches s->localPort==dstPort and, for a
	// connected socket, s->remoteIp/Port==srcIp/Port — so passing (ourIp, ourPort, peerIp, peerPort)
	// finds exactly our connection. It returns a connected socket only when the peer matches; a bare
	// wildcard listener comes back as `best` (connected==false), which we then ignore.
	Socket* s = socketLookupUdp(localIp, localPort, remoteIp, remotePort);
	if (s && s->connected) {
		s->soError = err;
		socketWakeReaders(s);            // unblock a parked recv so it can report the error
	}
}

int udpSend(Socket* s, const void* buf, unsigned len, uint32_t dstIp, uint16_t dstPort) {
	if (!s) return -SOCK_EINVAL;
	if (len > 65507) return -SOCK_EMSGSIZE;    // max UDP payload
	if (!s->bound) {                           // auto-bind a source port (Linux does on first send)
		s->localPort = socketEphemeralPort();
		s->bound = true;
	}
	// Source address for the pseudo-header = the egress interface's IP (route the destination).
	NetDevice* dev = 0; uint32_t nh = 0; uint32_t src = 0;
	if (routeLookup(dstIp, &dev, &nh) && dev) src = dev->ip;

	NetBuf* skb = netbufAlloc();
	if (!skb) return -SOCK_ENOBUFS;
	skb->reserve(NET_HEADROOM);
	unsigned char* payload = skb->put(len);
	if (len) memcpy(payload, buf, len);
	unsigned char* u = skb->push(UDP_HLEN);
	wr16be(u + 0, s->localPort);
	wr16be(u + 2, dstPort);
	wr16be(u + 4, (uint16_t) (UDP_HLEN + len));
	wr16be(u + 6, 0);
	uint16_t c = inetPseudoChecksum(src, dstIp, IPPROTO_UDP, u, UDP_HLEN + len);
	wr16be(u + 6, c ? c : 0xFFFF);             // 0 checksum is transmitted as 0xFFFF (RFC 768)
	if (ipOutput(dstIp, IPPROTO_UDP, skb) < 0)
		return -SOCK_ENOBUFS;
	g_netStats.udpOutDatagrams++;
	return (int) len;
}

}  // namespace kernel
