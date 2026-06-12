#include "Ip.h"
#include "Ether.h"
#include "Arp.h"
#include "Route.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include <string.h>

namespace kernel {

namespace {
IpProtoHandler g_proto[256];
uint16_t g_ipId = 1;                  // IPv4 identification counter (host order)

unsigned (*g_clock)() = 0;
unsigned now() { return g_clock ? g_clock() : 0; }

bool acceptForUs(NetDevice* dev, uint32_t dst) {
	if (!dev) return false;
	if (dst == 0xFFFFFFFFu) return true;                         // limited broadcast
	if (dev->flags & NETIF_LOOPBACK) return true;               // lo accepts its own traffic
	if (dev->ip != 0 && dst == dev->ip) return true;            // our unicast
	if (dev->broadcast && dst == dev->broadcast) return true;   // directed broadcast
	if (dev->ip == 0) return true;                              // unconfigured (DHCP) — accept
	return false;
}

// ---- fragment reassembly (bounded by NetBuf::CAP; ordering/MF/offset/timeout are complete) ----
struct Reasm {
	bool     used;
	uint32_t src, dst;
	uint16_t id;
	uint8_t  proto;
	uint8_t  ihl;
	unsigned char hdr[60];        // the first fragment's IP header (for the rebuilt datagram)
	unsigned char data[NetBuf::CAP];
	int      total;               // payload length once the last fragment is in, else -1
	int      received;
	unsigned lastTick;
};
const int REASM_N = 4;
const unsigned REASM_TIMEOUT = 30000;   // 30 s, like Linux IP fragment TTL
Reasm g_reasm[REASM_N];

Reasm* reasmFind(uint32_t src, uint32_t dst, uint16_t id, uint8_t proto) {
	for (int i = 0; i < REASM_N; i++)
		if (g_reasm[i].used && g_reasm[i].src == src && g_reasm[i].dst == dst &&
		    g_reasm[i].id == id && g_reasm[i].proto == proto)
			return &g_reasm[i];
	return 0;
}
Reasm* reasmAlloc(uint32_t src, uint32_t dst, uint16_t id, uint8_t proto) {
	for (int i = 0; i < REASM_N; i++)
		if (!g_reasm[i].used) {
			Reasm* r = &g_reasm[i];
			r->used = true; r->src = src; r->dst = dst; r->id = id; r->proto = proto;
			r->ihl = 0; r->total = -1; r->received = 0; r->lastTick = now();
			return r;
		}
	return 0;   // table full: drop (caller frees the skb)
}

// Returns a completed reassembled datagram (a fresh NetBuf), or 0 if still incomplete/stored.
// Consumes the input fragment either way.
NetBuf* ipReassemble(NetBuf* skb, uint32_t src, uint32_t dst, uint16_t id, uint8_t proto,
                     int ihl, int fragOff, bool mf, int total) {
	Reasm* r = reasmFind(src, dst, id, proto);
	if (!r) r = reasmAlloc(src, dst, id, proto);
	if (!r) { netbufFree(skb); return 0; }
	const unsigned char* h = skb->head();
	int payload = total - ihl;
	if (payload < 0 || fragOff + payload > (int) sizeof(r->data)) {   // bound: oversize -> drop ctx
		r->used = false; netbufFree(skb); return 0;
	}
	if (fragOff == 0) { r->ihl = (uint8_t) ihl; memcpy(r->hdr, h, ihl); }   // keep header of frag 0
	memcpy(r->data + fragOff, h + ihl, payload);
	r->received += payload;
	if (!mf) r->total = fragOff + payload;                                  // last fragment: total known
	r->lastTick = now();
	netbufFree(skb);

	if (r->total < 0 || r->received < r->total)
		return 0;                                                          // not complete yet

	// Rebuild a single datagram: header of fragment 0 (frag fields cleared) + the payload.
	NetBuf* out = netbufAlloc();
	if (!out) { r->used = false; return 0; }
	out->reserve(0);
	int ih = r->ihl ? r->ihl : IP_HLEN_MIN;
	unsigned char* o = out->put(ih + r->total);
	memcpy(o, r->hdr, ih);
	memcpy(o + ih, r->data, r->total);
	wr16be(o + 2, (uint16_t) (ih + r->total));   // total length
	wr16be(o + 6, 0);                            // clear flags + fragment offset
	wr16be(o + 10, 0);
	wr16be(o + 10, inetChecksum(o, ih));         // recompute header checksum
	r->used = false;                             // caller sets out->dev after we return
	return out;
}

void deliver(NetBuf* skb, uint32_t src, uint32_t dst, uint8_t proto, int ihl) {
	skb->saddr = src; skb->daddr = dst; skb->ipproto = proto;
	skb->l3 = skb->data;
	skb->pull(ihl);
	skb->l4 = skb->data;
	if (g_proto[proto]) g_proto[proto](skb);
	else netbufFree(skb);
}

// Build an IPv4 header in front of skb's payload and transmit one (already next-hop-known) frame.
void sendOne(NetDevice* dev, uint32_t nexthop, NetBuf* skb) {
	uint8_t mac[6];
	if (arpResolve(dev, nexthop, mac))
		ethSend(dev, skb, mac, ETH_P_IP);
	else
		arpHold(dev, nexthop, skb);   // request is out; flushed as IP when the reply lands
}

void buildHeader(NetBuf* skb, uint32_t src, uint32_t dst, uint8_t proto, uint16_t id,
                 uint16_t flagsFrag, int ttl) {
	int payload = skb->len;
	unsigned char* h = skb->push(IP_HLEN_MIN);
	h[0] = 0x45;                          // version 4, IHL 5
	h[1] = 0;                             // DSCP/ECN
	wr16be(h + 2, (uint16_t) (IP_HLEN_MIN + payload));
	wr16be(h + 4, id);
	wr16be(h + 6, flagsFrag);
	h[8] = (uint8_t) ttl;
	h[9] = proto;
	wr16be(h + 10, 0);                    // checksum field zeroed for the computation
	wr32be(h + 12, src);
	wr32be(h + 16, dst);
	wr16be(h + 10, inetChecksum(h, IP_HLEN_MIN));
}
}  // namespace

void ipSetHandler(uint8_t proto, IpProtoHandler h) { g_proto[proto] = h; }
void ipReasmSetClock(unsigned (*fn)()) { g_clock = fn; }

void ipInit() { ethSetIpHandler(ipRx); }

void ipRx(NetBuf* skb) {
	if (!skb) return;
	if (skb->len < IP_HLEN_MIN) { netbufFree(skb); return; }
	const unsigned char* h = skb->head();
	int ver = h[0] >> 4;
	int ihl = (h[0] & 0x0f) * 4;
	if (ver != 4 || ihl < IP_HLEN_MIN || skb->len < ihl) { netbufFree(skb); return; }
	int total = rd16be(h + 2);
	if (total < ihl || total > skb->len) { netbufFree(skb); return; }
	if (inetChecksum(h, ihl) != 0) { netbufFree(skb); return; }     // bad header checksum: drop
	skb->trim(total);                                               // strip any L2 padding

	NetDevice* dev = skb->dev;
	uint32_t src = rd32be(h + 12), dst = rd32be(h + 16);
	uint8_t proto = h[9];
	if (!acceptForUs(dev, dst)) { netbufFree(skb); return; }        // host: no forwarding

	uint16_t flags = rd16be(h + 6);
	int fragOff = (flags & IP_FRAG_MASK) * 8;
	bool mf = (flags & IP_FLAG_MF) != 0;
	if (mf || fragOff > 0) {
		uint16_t id = rd16be(h + 4);
		NetBuf* full = ipReassemble(skb, src, dst, id, proto, ihl, fragOff, mf, total);
		if (!full) return;                                         // stored or dropped
		full->dev = dev;
		const unsigned char* fh = full->head();
		ihl = (fh[0] & 0x0f) * 4;
		src = rd32be(fh + 12); dst = rd32be(fh + 16); proto = fh[9];
		deliver(full, src, dst, proto, ihl);
		return;
	}
	deliver(skb, src, dst, proto, ihl);
}

int ipOutput(uint32_t dst, uint8_t proto, NetBuf* skb, bool df) {
	if (!skb) return -1;
	NetDevice* dev = 0;
	uint32_t nexthop = 0;
	if (!routeLookup(dst, &dev, &nexthop) || !dev) { netbufFree(skb); return -1; }
	uint32_t src = dev->ip;
	uint16_t id = g_ipId++;

	int payload = skb->len;
	if (IP_HLEN_MIN + payload <= dev->mtu) {
		buildHeader(skb, src, dst, proto, id, df ? IP_FLAG_DF : 0, IP_DEFAULT_TTL);
		sendOne(dev, nexthop, skb);
		return 0;
	}

	// Fragment: split the payload into (mtu-20)-byte chunks rounded down to a multiple of 8.
	int maxData = (dev->mtu - IP_HLEN_MIN) & ~7;
	const unsigned char* p = skb->head();
	int off = 0;
	while (off < payload) {
		int chunk = payload - off;
		bool last = true;
		if (chunk > maxData) { chunk = maxData; last = false; }
		NetBuf* frag = netbufAlloc();
		if (!frag) break;                                          // OOM mid-fragmentation: drop rest
		frag->reserve(NET_HEADROOM);
		memcpy(frag->put(chunk), p + off, chunk);
		uint16_t flagsFrag = (uint16_t) ((off / 8) & IP_FRAG_MASK);
		if (!last) flagsFrag |= IP_FLAG_MF;
		buildHeader(frag, src, dst, proto, id, flagsFrag, IP_DEFAULT_TTL);
		sendOne(dev, nexthop, frag);
		off += chunk;
	}
	netbufFree(skb);
	return 0;
}

void ipReasmTick(unsigned t) {
	for (int i = 0; i < REASM_N; i++)
		if (g_reasm[i].used && (t - g_reasm[i].lastTick) > REASM_TIMEOUT)
			g_reasm[i].used = false;   // incomplete datagram expired (Linux drops + ICMP time-exceeded)
}

void ipReset() {
	for (int i = 0; i < 256; i++) g_proto[i] = 0;
	for (int i = 0; i < REASM_N; i++) g_reasm[i].used = false;
	g_ipId = 1; g_clock = 0;
}

}  // namespace kernel
