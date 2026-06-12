/*
 * Ether.h — Ethernet II framing + L2 demux. ethInit() installs ethRx as the net core's input
 * handler; ethRx parses the 14-byte header, sets skb->protocol, strips it, and dispatches to
 * the registered L3 handler (ARP in FAZA 4, IP in FAZA 5). ethSend prepends the header (src =
 * device MAC), pads to the 60-byte minimum like Linux, and transmits.
 */
#pragma once
#include <stdint.h>
#include "NetBuf.h"

namespace kernel {

struct NetDevice;

enum {
	ETH_ALEN = 6,
	ETH_HLEN = 14,
	ETH_MIN  = 60,          // minimum Ethernet frame (excl. FCS); shorter frames are zero-padded
	ETH_P_IP  = 0x0800,
	ETH_P_ARP = 0x0806,
};

// Standard TX headroom every builder reserves so lower layers can push() their headers without
// copying: Ethernet 14 + IPv4 60 (max) + TCP 60 (max), rounded up.
enum { NET_HEADROOM = 144 };

// A registered L3 protocol handler. On entry skb->head() points at the L3 payload (header
// stripped), skb->dev is set, skb->protocol is the ethertype. The handler owns the skb.
typedef void (*EthProtoHandler)(NetBuf* skb);

void ethInit();                                  // install ethRx as the net input handler
void ethSetArpHandler(EthProtoHandler h);        // ARP installs its receiver (FAZA 4)
void ethSetIpHandler(EthProtoHandler h);         // IP installs its receiver (FAZA 5)

// The L2 input handler (installed by ethInit). Demuxes by ethertype. Owns the skb.
void ethRx(NetBuf* skb);

// Prepend an Ethernet header (dst, src = dev->mac, ethertype) to skb (which holds the L3
// payload at head()), pad to ETH_MIN, and transmit. Consumes the skb. Returns netTransmit's rc.
int ethSend(NetDevice* dev, NetBuf* skb, const uint8_t dst[6], uint16_t ethertype);

// True if mac is the all-ones broadcast address.
bool ethIsBroadcast(const uint8_t mac[6]);

}  // namespace kernel
