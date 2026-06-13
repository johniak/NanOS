#include "Raw.h"
#include "Socket.h"
#include "Icmp.h"
#include "Ip.h"
#include "Ether.h"     // NET_HEADROOM
#include "NetBuf.h"
#include <string.h>

namespace kernel {

namespace {
// Deliver a copy of the (full-IP) ICMP datagram to one raw socket. socketForEachRaw callback.
void deliverCopy(Socket* s, void* ctx) {
	NetBuf* src = (NetBuf*) ctx;
	NetBuf* copy = netbufAlloc();
	if (!copy) return;
	copy->reserve(0);
	memcpy(copy->put(src->len), src->head(), src->len);
	if (!socketDeliver(s, copy, src->saddr, 0))   // raw has no port; srcPort 0
		netbufFree(copy);
}
}  // namespace

// ICMP raw hook: skb->head() is the ICMP message and skb->l3 still marks the IP header (set by
// the IP demux). Restore the head to the IP header so raw sockets see the whole datagram (what
// inetutils ping expects: recvfrom returns 20-byte IP + ICMP). Then fan a copy to each raw
// ICMP socket and free the original.
static void rawIcmpRx(NetBuf* skb) {
	if (!skb) return;
	if (skb->l3 >= 0 && skb->l3 <= skb->data) {
		skb->len += (skb->data - skb->l3);
		skb->data = skb->l3;                      // head() now points at the IP header
	}
	socketForEachRaw(IPPROTO_ICMP, deliverCopy, skb);
	netbufFree(skb);
}

void rawInit() { icmpSetRawHandler(rawIcmpRx); }

int rawSend(Socket* s, const void* buf, unsigned len, uint32_t dstIp, uint16_t /*dstPort*/) {
	if (!s) return -SOCK_EINVAL;
	uint8_t proto = (uint8_t) (s->protocol ? s->protocol : IPPROTO_ICMP);
	NetBuf* skb = netbufAlloc();
	if (!skb) return -SOCK_ENOBUFS;
	skb->reserve(NET_HEADROOM);
	if (len) memcpy(skb->put(len), buf, len);     // caller (ping) supplies the full ICMP message
	if (ipOutput(dstIp, proto, skb, false, s->ttl) < 0)
		return -SOCK_ENOBUFS;
	return (int) len;
}

}  // namespace kernel
