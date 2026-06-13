/*
 * Packet.cpp — AF_PACKET socket core (see Packet.h). Pure MI: the tap copies frames into the
 * matching sockets' receive rings; TX builds (cooked) or forwards (raw) an L2 frame straight to
 * the device, bypassing routing — which is exactly what DHCP needs before the interface has an IP.
 */
#include "Packet.h"
#include "Socket.h"
#include "NetBuf.h"
#include "NetDevice.h"
#include "Ether.h"
#include "Net.h"
#include <string.h>

namespace kernel {

// ifindex = device-registry position + 1 (so a valid ifindex is always >= 1).
int netIfIndexOf(NetDevice* dev) {
	for (int i = 0; i < netCount(); i++)
		if (netByIndex(i) == dev) return i + 1;
	return 0;
}
NetDevice* netByIfIndex(int idx) { return idx >= 1 ? netByIndex(idx - 1) : 0; }

// Linux PACKET_* pkttypes, derived from the destination MAC relative to the receiving device.
enum { PACKET_HOST = 0, PACKET_BROADCAST = 1, PACKET_MULTICAST = 2, PACKET_OTHERHOST = 3 };
static bool maceq(const unsigned char* a, const unsigned char* b) {   // memcmp isn't freestanding
	for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
	return true;
}
static int pkttype_of(const unsigned char* dmac, NetDevice* dev) {
	static const unsigned char bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
	if (maceq(dmac, bcast)) return PACKET_BROADCAST;
	if (dmac[0] & 1) return PACKET_MULTICAST;
	if (dev && maceq(dmac, dev->mac)) return PACKET_HOST;
	return PACKET_OTHERHOST;
}

int packetBind(Socket* s, int ifindex, uint16_t protocol) {
	if (!s) return -SOCK_EINVAL;
	s->localIp = (uint32_t) ifindex;            // bound ifindex (0 = any interface)
	if (protocol) s->protocol = protocol;        // optionally rebind the EtherType filter (net order)
	s->bound = true;
	return 0;
}

void packetRxTap(NetBuf* frame) {
	if (!frame || frame->len < ETH_HLEN) return;
	const unsigned char* h = frame->head();
	uint16_t type = rd16be(h + 12);              // ethertype, host order
	NetDevice* dev = frame->dev;
	int ifindex = netIfIndexOf(dev);
	for (int i = 0; i < socketSlots(); i++) {
		Socket* s = socketAt(i);
		if (!s || s->domain != AF_PACKET) continue;
		uint16_t want = ntoh16((uint16_t) s->protocol);          // filter, host order
		if (want != 0 && want != ETH_P_ALL && want != type) continue;
		if (s->localIp != 0 && (int) s->localIp != ifindex) continue;
		// Deliver a copy of the WHOLE frame; recv() returns it raw or strips the L2 header for
		// cooked sockets and rebuilds the sockaddr_ll from the frame bytes + skb->dev.
		NetBuf* c = netbufAlloc();
		if (!c) continue;
		c->reserve(0);
		memcpy(c->put(frame->len), h, frame->len);
		c->dev = dev;
		if (!socketDeliver(s, c, 0, 0)) netbufFree(c);   // ring full: drop (packet sockets lossy)
	}
}

int packetSend(Socket* s, const void* buf, unsigned len, int ifindex, uint16_t protocol,
               const unsigned char* dmac) {
	if (!s) return -SOCK_EINVAL;
	int idx = ifindex ? ifindex : (int) s->localIp;
	NetDevice* dev = idx ? netByIfIndex(idx) : netPrimary();
	if (!dev) return -SOCK_ENETUNREACH;
	NetBuf* skb = netbufAlloc();
	if (!skb) return -SOCK_ENOBUFS;
	skb->reserve(NET_HEADROOM);
	if (s->type == SOCK_RAW) {
		memcpy(skb->put(len), buf, len);                 // the buffer IS the full L2 frame
	} else {
		unsigned char* e = skb->put(ETH_HLEN + len);     // cooked: build the Ethernet header
		static const unsigned char bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
		memcpy(e, dmac ? dmac : bcast, 6);               // dst from sockaddr_ll (bcast if absent)
		memcpy(e + 6, dev->mac, 6);                      // src = our MAC
		wr16be(e + 12, ntoh16(protocol));                // sll_protocol (net order) -> wire ethertype
		memcpy(e + ETH_HLEN, buf, len);
	}
	skb->dev = dev;
	int rc = netTransmit(dev, skb);                      // straight to the device, no routing
	return rc < 0 ? -SOCK_ENOBUFS : (int) len;
}

bool packetReadable(Socket* s) { return s && s->rxCount > 0; }

int packetRecv(Socket* s, void* buf, unsigned len, int flags,
               int* ifindexOut, uint16_t* protoOut, int* pkttypeOut, unsigned char macOut[8]) {
	if (!s) return -SOCK_EINVAL;
	if (s->rxCount == 0) return -SOCK_EAGAIN;
	NetBuf* skb = s->rxq[s->rxTail].skb;
	const unsigned char* fr = skb->head();
	int frlen = skb->len;
	int off = (s->type == SOCK_RAW) ? 0 : ETH_HLEN;      // cooked sockets see the L3 payload
	int avail = frlen - off; if (avail < 0) avail = 0;
	int n = (int) len < avail ? (int) len : avail;
	memcpy(buf, fr + off, n);
	if (ifindexOut) *ifindexOut = netIfIndexOf(skb->dev);
	if (protoOut)   *protoOut   = hton16(rd16be(fr + 12));   // report sll_protocol in network order
	if (pkttypeOut) *pkttypeOut = pkttype_of(fr, skb->dev);
	if (macOut) { memset(macOut, 0, 8); memcpy(macOut, fr + 6, 6); }
	if (!(flags & MSG_PEEK)) {
		s->rxBytes -= skb->len;
		netbufFree(skb);
		s->rxTail = (s->rxTail + 1) % Socket::RXQ;
		s->rxCount--;
	}
	return n;
}

}  // namespace kernel
