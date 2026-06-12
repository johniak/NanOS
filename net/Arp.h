/*
 * Arp.h — Address Resolution Protocol (IPv4-over-Ethernet): the neighbor cache (IP<->MAC) plus
 * request/reply and the packet queue for addresses still being resolved. Linux-faithful wire
 * format and cache states (INCOMPLETE/REACHABLE/STALE). arpInit() registers arpRx with Ether.
 *
 * IP output path (FAZA 5): call arpResolve(dev, nexthop, mac); if it returns true, send now;
 * if false, a request is already on the wire — hand the packet to arpHold, and arpRx flushes it
 * when the reply lands. All addresses are host byte order internally.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct NetDevice;
struct NetBuf;

enum {
	ARP_HTYPE_ETH = 1,
	ARP_OP_REQUEST = 1,
	ARP_OP_REPLY   = 2,
	ARP_PLEN = 28,            // IPv4/Ethernet ARP payload size
};

enum ArpState { ARP_FREE = 0, ARP_INCOMPLETE, ARP_REACHABLE, ARP_STALE };

struct ArpEntry {
	uint32_t ip;              // host order
	uint8_t  mac[6];
	uint8_t  state;
	unsigned lastTick;        // for aging (when a clock is installed)
	int      probes;          // INCOMPLETE retransmit count
};

void arpInit();               // register arpRx with Ether
void arpRx(NetBuf* skb);      // L2 handler: skb->head() = ARP payload, skb->dev set. Owns skb.

// Resolve ip -> mac on dev. Returns true + fills mac if REACHABLE; otherwise sends a request
// (creating/refreshing an INCOMPLETE entry) and returns false.
bool arpResolve(NetDevice* dev, uint32_t ip, uint8_t out[6]);

// Queue an IP packet (skb holds the IP datagram at head()) pending resolution of ip; arpRx
// transmits it once the reply arrives. Takes ownership of skb.
void arpHold(NetDevice* dev, uint32_t ip, NetBuf* skb);

// Send a broadcast ARP request for target_ip out dev.
void arpRequest(NetDevice* dev, uint32_t target_ip);

// Aging/retransmit pass (call periodically with the current tick). Re-sends INCOMPLETE probes,
// expires REACHABLE -> STALE, drops dead entries + their queued packets. Pure given `now`.
void arpTick(unsigned now);

// Clock hook (returns a monotonically increasing tick). The kernel installs Scheduler::ticks;
// tests inject a controllable clock. Default returns 0 (no aging).
typedef unsigned (*ArpClockFn)();
void arpSetClock(ArpClockFn fn);

// Introspection (for /proc/net/arp + tests).
const ArpEntry* arpLookup(uint32_t ip);
int  arpCacheCount();
int  arpSlots();                       // total cache slots (iterate 0..arpSlots()-1)
const ArpEntry* arpEntryAt(int slot);  // raw slot (may be ARP_FREE); 0 if out of range
void arpReset();              // clear cache + pending queue (tests)

}  // namespace kernel
