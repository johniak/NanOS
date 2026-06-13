#include "Ether.h"
#include "NetDevice.h"
#include "Net.h"
#include "Packet.h"   // packetRxTap (AF_PACKET RX tap)
#include <string.h>

namespace kernel {

namespace {
EthProtoHandler g_arp = 0;
EthProtoHandler g_ip  = 0;
const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
}  // namespace

void ethSetArpHandler(EthProtoHandler h) { g_arp = h; }
void ethSetIpHandler(EthProtoHandler h)  { g_ip = h; }

bool ethIsBroadcast(const uint8_t mac[6]) {
	for (int i = 0; i < 6; i++) if (mac[i] != 0xff) return false;
	return true;
}

void ethRx(NetBuf* skb) {
	if (!skb) return;
	if (skb->len < ETH_HLEN) { netbufFree(skb); return; }   // runt: drop (hardening)
	packetRxTap(skb);             // AF_PACKET tap on the full frame, before the L2 header is stripped
	const unsigned char* h = skb->head();
	// h[0..5] dst, h[6..11] src, h[12..13] ethertype.
	uint16_t type = rd16be(h + 12);
	skb->protocol = type;
	skb->pull(ETH_HLEN);          // strip the L2 header; head() now at the L3 payload
	skb->l3 = skb->data;
	switch (type) {
	case ETH_P_ARP: if (g_arp) { g_arp(skb); return; } break;
	case ETH_P_IP:  if (g_ip)  { g_ip(skb);  return; } break;
	default: break;
	}
	netbufFree(skb);              // unknown ethertype or no handler installed yet
}

int ethSend(NetDevice* dev, NetBuf* skb, const uint8_t dst[6], uint16_t ethertype) {
	if (!dev || !skb) { if (skb) netbufFree(skb); return -1; }
	unsigned char* h = skb->push(ETH_HLEN);
	memcpy(h, dst, 6);
	memcpy(h + 6, dev->mac, 6);
	wr16be(h + 12, ethertype);
	// Pad to the 60-byte minimum (Linux/hardware do this); the e1000 appends the 4-byte FCS.
	if (skb->len < ETH_MIN) {
		int pad = ETH_MIN - skb->len;
		memset(skb->put(pad), 0, pad);
	}
	return netTransmit(dev, skb);
}

void ethInit() {
	(void) BCAST;
	netSetInputHandler(ethRx);
}

}  // namespace kernel
