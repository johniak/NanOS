#include "Arp.h"
#include "Ether.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include <string.h>

namespace kernel {

namespace {
const int CACHE_N = 16;
ArpEntry g_cache[CACHE_N];

struct Pending { NetDevice* dev; uint32_t ip; NetBuf* skb; };
const int PEND_N = 16;
Pending g_pend[PEND_N];

ArpClockFn g_clock = 0;
unsigned now() { return g_clock ? g_clock() : 0; }

// Aging thresholds (ticks; the kernel clock is 1 kHz so these are ms).
const unsigned REACHABLE_MS = 30000;
const unsigned PROBE_MS = 1000;
const int MAX_PROBES = 3;

ArpEntry* find(uint32_t ip) {
	for (int i = 0; i < CACHE_N; i++)
		if (g_cache[i].state != ARP_FREE && g_cache[i].ip == ip)
			return &g_cache[i];
	return 0;
}
ArpEntry* findFree() {
	for (int i = 0; i < CACHE_N; i++)
		if (g_cache[i].state == ARP_FREE)
			return &g_cache[i];
	// Evict the oldest REACHABLE/STALE (simple: first non-INCOMPLETE).
	for (int i = 0; i < CACHE_N; i++)
		if (g_cache[i].state != ARP_INCOMPLETE)
			return &g_cache[i];
	return &g_cache[0];
}

// Build + send an ARP packet (request or reply). dstMac = where the Ethernet frame goes.
void arpSend(NetDevice* dev, uint16_t op, const uint8_t* targetMac, uint32_t targetIp,
             const uint8_t* dstMac) {
	NetBuf* skb = netbufAlloc();
	if (!skb)
		return;
	skb->reserve(NET_HEADROOM);
	unsigned char* a = skb->put(ARP_PLEN);
	wr16be(a + 0, ARP_HTYPE_ETH);
	wr16be(a + 2, ETH_P_IP);
	a[4] = 6;                          // hlen
	a[5] = 4;                          // plen
	wr16be(a + 6, op);
	memcpy(a + 8, dev->mac, 6);        // sender hardware = us
	wr32be(a + 14, dev->ip);           // sender protocol = our IP
	memcpy(a + 18, targetMac, 6);      // target hardware (zero for a request)
	wr32be(a + 24, targetIp);          // target protocol
	ethSend(dev, skb, dstMac, ETH_P_ARP);
}

// Flush packets queued for ip now that we have its MAC.
void flushPending(NetDevice* dev, uint32_t ip, const uint8_t mac[6]) {
	for (int i = 0; i < PEND_N; i++) {
		if (g_pend[i].skb && g_pend[i].ip == ip && g_pend[i].dev == dev) {
			NetBuf* skb = g_pend[i].skb;
			g_pend[i].skb = 0;
			ethSend(dev, skb, mac, ETH_P_IP);
		}
	}
}

void learn(NetDevice* dev, uint32_t ip, const uint8_t mac[6], bool create) {
	ArpEntry* e = find(ip);
	if (!e) {
		if (!create)
			return;
		e = findFree();
		// If we're evicting an INCOMPLETE-free slot that held a different ip, drop its queue.
		e->ip = ip;
		e->probes = 0;
	}
	memcpy(e->mac, mac, 6);
	e->ip = ip;
	e->state = ARP_REACHABLE;
	e->lastTick = now();
	flushPending(dev, ip, mac);
}
}  // namespace

void arpSetClock(ArpClockFn fn) { g_clock = fn; }

void arpInit() { ethSetArpHandler(arpRx); }

void arpRx(NetBuf* skb) {
	if (!skb) return;
	NetDevice* dev = skb->dev;
	if (!dev || skb->len < ARP_PLEN) { netbufFree(skb); return; }   // runt: drop (hardening)
	const unsigned char* a = skb->head();
	uint16_t htype = rd16be(a + 0), ptype = rd16be(a + 2);
	uint16_t op = rd16be(a + 6);
	if (htype != ARP_HTYPE_ETH || ptype != ETH_P_IP || a[4] != 6 || a[5] != 4) {
		netbufFree(skb);                                            // not IPv4-over-Ethernet ARP
		return;
	}
	uint8_t sha[6]; memcpy(sha, a + 8, 6);
	uint32_t spa = rd32be(a + 14);
	uint32_t tpa = rd32be(a + 24);
	bool forUs = (tpa == dev->ip && dev->ip != 0);

	// Learn the sender: update any existing entry; create only if this ARP targets us or is a
	// reply (Linux-style — don't cache the whole broadcast domain from unrelated requests).
	learn(dev, spa, sha, /*create*/ forUs || op == ARP_OP_REPLY);

	if (op == ARP_OP_REQUEST && forUs)
		arpSend(dev, ARP_OP_REPLY, sha, spa, sha);   // unicast reply to the requester

	netbufFree(skb);
}

void arpRequest(NetDevice* dev, uint32_t target_ip) {
	static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static const uint8_t zero[6]  = { 0, 0, 0, 0, 0, 0 };
	arpSend(dev, ARP_OP_REQUEST, zero, target_ip, bcast);
}

bool arpResolve(NetDevice* dev, uint32_t ip, uint8_t out[6]) {
	ArpEntry* e = find(ip);
	if (e && e->state == ARP_REACHABLE) {
		memcpy(out, e->mac, 6);
		return true;
	}
	if (!e) {
		e = findFree();
		memset(e, 0, sizeof(*e));
		e->ip = ip;
		e->state = ARP_INCOMPLETE;
		e->probes = 1;
		e->lastTick = now();
		arpRequest(dev, ip);
	}
	return false;
}

void arpHold(NetDevice* dev, uint32_t ip, NetBuf* skb) {
	for (int i = 0; i < PEND_N; i++) {
		if (!g_pend[i].skb) {
			g_pend[i].dev = dev; g_pend[i].ip = ip; g_pend[i].skb = skb;
			return;
		}
	}
	netbufFree(skb);   // queue full: drop (caller's packet is lost, like Linux under pressure)
}

void arpTick(unsigned t) {
	for (int i = 0; i < CACHE_N; i++) {
		ArpEntry* e = &g_cache[i];
		if (e->state == ARP_REACHABLE && (t - e->lastTick) > REACHABLE_MS) {
			e->state = ARP_STALE;
		} else if (e->state == ARP_INCOMPLETE && (t - e->lastTick) > PROBE_MS) {
			if (e->probes >= MAX_PROBES) {
				// Resolution failed: drop the entry + its queued packets.
				for (int j = 0; j < PEND_N; j++)
					if (g_pend[j].skb && g_pend[j].ip == e->ip) { netbufFree(g_pend[j].skb); g_pend[j].skb = 0; }
				e->state = ARP_FREE;
			} else {
				e->probes++;
				e->lastTick = t;
				// Re-send the request on the device of any queued packet for this ip.
				for (int j = 0; j < PEND_N; j++)
					if (g_pend[j].skb && g_pend[j].ip == e->ip) { arpRequest(g_pend[j].dev, e->ip); break; }
			}
		}
	}
}

const ArpEntry* arpLookup(uint32_t ip) { return find(ip); }
int arpCacheCount() {
	int n = 0;
	for (int i = 0; i < CACHE_N; i++) if (g_cache[i].state != ARP_FREE) n++;
	return n;
}
void arpReset() {
	for (int i = 0; i < CACHE_N; i++) g_cache[i].state = ARP_FREE;
	for (int i = 0; i < PEND_N; i++) { if (g_pend[i].skb) netbufFree(g_pend[i].skb); g_pend[i].skb = 0; }
	g_clock = 0;
}

}  // namespace kernel
