#include "doctest.h"
#include "Ether.h"
#include "Arp.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include <cstdint>
#include <cstring>

using namespace kernel;

// ---- capture harness: a mock device whose tx records the emitted frame ----
static unsigned char g_cap[2048];
static int g_capLen;
static int g_capCount;
static int captureTx(NetDevice*, NetBuf* skb) {
	g_capLen = skb->len;
	std::memcpy(g_cap, skb->head(), skb->len < 2048 ? skb->len : 2048);
	g_capCount++;
	netbufFree(skb);
	return 0;
}
static void clearCap() { g_capLen = 0; g_capCount = 0; std::memset(g_cap, 0, sizeof(g_cap)); }

static const uint8_t OUR_MAC[6] = { 0xaa, 0xbb, 0xcc, 0x00, 0x00, 0x01 };
static const uint8_t REQ_MAC[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
static const uint8_t GW_MAC[6]  = { 0x52, 0x54, 0x00, 0x12, 0x35, 0x02 };

static NetDevice makeEth() {
	NetDevice d; std::memset(&d, 0, sizeof(d));
	d.name[0] = 'e'; d.name[1] = 't'; d.name[2] = 'h'; d.name[3] = '0';
	std::memcpy(d.mac, OUR_MAC, 6);
	d.mtu = 1500; d.flags = NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST;
	d.ip = ipv4(10, 0, 2, 15);
	d.tx = captureTx;
	return d;
}

// Build an Ethernet frame carrying an ARP packet and feed it to the stack (ethRx).
static void feedArp(NetDevice* dev, const uint8_t dstMac[6], const uint8_t srcMac[6],
                    uint16_t op, const uint8_t sha[6], uint32_t spa,
                    const uint8_t tha[6], uint32_t tpa) {
	NetBuf* skb = netbufAlloc();
	skb->reserve(0);
	unsigned char* e = skb->put(ETH_HLEN + ARP_PLEN);
	std::memcpy(e, dstMac, 6);
	std::memcpy(e + 6, srcMac, 6);
	wr16be(e + 12, ETH_P_ARP);
	unsigned char* a = e + ETH_HLEN;
	wr16be(a + 0, ARP_HTYPE_ETH);
	wr16be(a + 2, ETH_P_IP);
	a[4] = 6; a[5] = 4;
	wr16be(a + 6, op);
	std::memcpy(a + 8, sha, 6);
	wr32be(a + 14, spa);
	std::memcpy(a + 18, tha, 6);
	wr32be(a + 24, tpa);
	skb->dev = dev;
	ethRx(skb);
}

static void setup(NetDevice& dev) {
	netReset();
	arpReset();
	ethInit();        // installs ethRx as the input handler
	arpInit();        // installs arpRx with Ether
	clearCap();
	dev = makeEth();
}

// ---------------------------------------------------------------------------

TEST_CASE("ARP request for us -> byte-exact reply + learns the requester") {
	NetDevice dev; setup(dev);
	const uint8_t bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
	const uint8_t zero[6]  = { 0,0,0,0,0,0 };
	feedArp(&dev, bcast, REQ_MAC, ARP_OP_REQUEST, REQ_MAC, ipv4(10,0,2,1), zero, ipv4(10,0,2,15));

	REQUIRE(g_capCount == 1);
	CHECK(g_capLen == 60);                                   // padded to the Ethernet minimum
	// Ethernet: dst = requester, src = us, type = ARP.
	CHECK(std::memcmp(g_cap + 0, REQ_MAC, 6) == 0);
	CHECK(std::memcmp(g_cap + 6, OUR_MAC, 6) == 0);
	CHECK(rd16be(g_cap + 12) == ETH_P_ARP);
	// ARP reply body.
	const unsigned char* a = g_cap + ETH_HLEN;
	CHECK(rd16be(a + 0) == ARP_HTYPE_ETH);
	CHECK(rd16be(a + 2) == ETH_P_IP);
	CHECK(a[4] == 6); CHECK(a[5] == 4);
	CHECK(rd16be(a + 6) == ARP_OP_REPLY);
	CHECK(std::memcmp(a + 8, OUR_MAC, 6) == 0);              // sender hw = us
	CHECK(rd32be(a + 14) == ipv4(10,0,2,15));               // sender proto = our ip
	CHECK(std::memcmp(a + 18, REQ_MAC, 6) == 0);            // target hw = requester
	CHECK(rd32be(a + 24) == ipv4(10,0,2,1));               // target proto = requester ip
	// We learned the requester.
	const ArpEntry* e = arpLookup(ipv4(10,0,2,1));
	REQUIRE(e != nullptr);
	CHECK(e->state == ARP_REACHABLE);
	CHECK(std::memcmp(e->mac, REQ_MAC, 6) == 0);
	netReset(); arpReset();
}

TEST_CASE("ARP request for someone else -> no reply, not cached") {
	NetDevice dev; setup(dev);
	const uint8_t bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
	const uint8_t zero[6]  = { 0,0,0,0,0,0 };
	feedArp(&dev, bcast, REQ_MAC, ARP_OP_REQUEST, REQ_MAC, ipv4(10,0,2,1), zero, ipv4(10,0,2,99));
	CHECK(g_capCount == 0);
	CHECK(arpCacheCount() == 0);                            // don't cache unrelated broadcast ARPs
	netReset(); arpReset();
}

TEST_CASE("ARP reply learns the sender, emits nothing") {
	NetDevice dev; setup(dev);
	feedArp(&dev, OUR_MAC, GW_MAC, ARP_OP_REPLY, GW_MAC, ipv4(10,0,2,2), OUR_MAC, ipv4(10,0,2,15));
	CHECK(g_capCount == 0);
	const ArpEntry* e = arpLookup(ipv4(10,0,2,2));
	REQUIRE(e != nullptr);
	CHECK(e->state == ARP_REACHABLE);
	CHECK(std::memcmp(e->mac, GW_MAC, 6) == 0);
	netReset(); arpReset();
}

TEST_CASE("arpResolve: miss sends a byte-exact broadcast request, no duplicate while INCOMPLETE") {
	NetDevice dev; setup(dev);
	uint8_t mac[6];
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == false);
	REQUIRE(g_capCount == 1);
	CHECK(g_capLen == 60);
	// Ethernet broadcast, src us, type ARP.
	const uint8_t bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
	CHECK(std::memcmp(g_cap + 0, bcast, 6) == 0);
	CHECK(std::memcmp(g_cap + 6, OUR_MAC, 6) == 0);
	CHECK(rd16be(g_cap + 12) == ETH_P_ARP);
	const unsigned char* a = g_cap + ETH_HLEN;
	CHECK(rd16be(a + 6) == ARP_OP_REQUEST);
	CHECK(std::memcmp(a + 8, OUR_MAC, 6) == 0);             // sender hw = us
	CHECK(rd32be(a + 14) == ipv4(10,0,2,15));              // sender proto = our ip
	CHECK(rd32be(a + 24) == ipv4(10,0,2,2));               // target proto = the address we want
	// A second resolve while INCOMPLETE must NOT re-broadcast.
	clearCap();
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == false);
	CHECK(g_capCount == 0);
	// A reply makes the next resolve a hit.
	feedArp(&dev, OUR_MAC, GW_MAC, ARP_OP_REPLY, GW_MAC, ipv4(10,0,2,2), OUR_MAC, ipv4(10,0,2,15));
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == true);
	CHECK(std::memcmp(mac, GW_MAC, 6) == 0);
	netReset(); arpReset();
}

TEST_CASE("pending packet is flushed (as an IP frame) when the reply arrives") {
	NetDevice dev; setup(dev);
	uint8_t mac[6];
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == false);   // sends the request
	clearCap();
	// Queue a fake IP datagram for the unresolved next-hop.
	NetBuf* ip = netbufAlloc();
	ip->reserve(NET_HEADROOM);
	unsigned char* p = ip->put(40);
	for (int i = 0; i < 40; i++) p[i] = (unsigned char) (0x45 + i);
	arpHold(&dev, ipv4(10,0,2,2), ip);
	CHECK(g_capCount == 0);                                 // still waiting
	// Reply arrives -> the held packet goes out as an IP frame to the gateway MAC.
	feedArp(&dev, OUR_MAC, GW_MAC, ARP_OP_REPLY, GW_MAC, ipv4(10,0,2,2), OUR_MAC, ipv4(10,0,2,15));
	REQUIRE(g_capCount == 1);
	CHECK(std::memcmp(g_cap + 0, GW_MAC, 6) == 0);          // dst = resolved MAC
	CHECK(std::memcmp(g_cap + 6, OUR_MAC, 6) == 0);
	CHECK(rd16be(g_cap + 12) == ETH_P_IP);
	CHECK(g_cap[ETH_HLEN] == 0x45);                         // our IP payload, first byte
	netReset(); arpReset();
}

TEST_CASE("runt frames are dropped without crashing (hardening)") {
	NetDevice dev; setup(dev);
	// 10-byte 'Ethernet' frame: shorter than the header -> drop.
	NetBuf* a = netbufAlloc(); a->reserve(0); a->put(10); a->dev = &dev;
	ethRx(a);
	CHECK(g_capCount == 0);
	// A valid Ethernet header but a 5-byte ARP body -> ARP drops it.
	NetBuf* b = netbufAlloc(); b->reserve(0);
	unsigned char* e = b->put(ETH_HLEN + 5);
	std::memset(e, 0, ETH_HLEN + 5);
	wr16be(e + 12, ETH_P_ARP);
	b->dev = &dev;
	ethRx(b);
	CHECK(g_capCount == 0);
	CHECK(netbufInUse() == 0);
	netReset(); arpReset();
}

// ---- Ethernet layer coverage ----

static int g_ipSeen, g_ipLen;
static void captureIp(NetBuf* skb) { g_ipSeen++; g_ipLen = skb->len; netbufFree(skb); }

TEST_CASE("ethIsBroadcast distinguishes broadcast from unicast") {
	const uint8_t b[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
	const uint8_t u[6] = { 0xff,0xff,0xff,0xff,0xff,0xfe };
	CHECK(ethIsBroadcast(b) == true);
	CHECK(ethIsBroadcast(u) == false);
}

TEST_CASE("ethRx dispatches IP frames to the IP handler; drops unknown ethertypes") {
	NetDevice dev; setup(dev);
	g_ipSeen = 0; g_ipLen = 0;
	ethSetIpHandler(captureIp);

	// An IP frame (ethertype 0x0800) with a 30-byte payload.
	NetBuf* a = netbufAlloc(); a->reserve(0);
	unsigned char* e = a->put(ETH_HLEN + 30);
	std::memset(e, 0, ETH_HLEN + 30);
	wr16be(e + 12, ETH_P_IP);
	a->dev = &dev;
	ethRx(a);
	CHECK(g_ipSeen == 1);
	CHECK(g_ipLen == 30);                    // header stripped, payload handed up

	// An unknown ethertype is dropped (no handler, freed).
	NetBuf* b = netbufAlloc(); b->reserve(0);
	unsigned char* e2 = b->put(ETH_HLEN + 30);
	std::memset(e2, 0, ETH_HLEN + 30);
	wr16be(e2 + 12, 0x9999);
	b->dev = &dev;
	ethRx(b);
	CHECK(g_ipSeen == 1);                    // unchanged
	CHECK(netbufInUse() == 0);
	ethSetIpHandler(nullptr);
	netReset(); arpReset();
}

TEST_CASE("ethSend rejects null args without leaking") {
	CHECK(ethSend(nullptr, nullptr, OUR_MAC, ETH_P_IP) == -1);
	NetDevice dev; setup(dev);
	CHECK(ethSend(nullptr, nullptr, OUR_MAC, ETH_P_IP) == -1);
	NetBuf* skb = netbufAlloc(); skb->reserve(NET_HEADROOM); skb->put(8);
	CHECK(ethSend(nullptr, skb, OUR_MAC, ETH_P_IP) == -1);   // frees skb on null dev
	CHECK(netbufInUse() == 0);
	netReset(); arpReset();
}

TEST_CASE("ethSend does not pad a frame already at/over the minimum") {
	NetDevice dev; setup(dev);
	NetBuf* skb = netbufAlloc(); skb->reserve(NET_HEADROOM);
	skb->put(60);                            // 60 + 14 header = 74 bytes, over ETH_MIN
	ethSend(&dev, skb, GW_MAC, ETH_P_IP);
	CHECK(g_capLen == 74);                   // no padding applied
	netReset(); arpReset();
}

static unsigned g_fakeNow;
static unsigned fakeClock() { return g_fakeNow; }

TEST_CASE("arpTick: INCOMPLETE retransmits then fails; REACHABLE goes STALE") {
	NetDevice dev; setup(dev);
	arpSetClock(fakeClock);
	g_fakeNow = 0;
	uint8_t mac[6];
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == false);   // probe 1, lastTick=0
	clearCap();
	// Advance past the probe interval a few times -> retransmits, then gives up.
	g_fakeNow = 1500; arpTick(g_fakeNow);                    // probe 2
	CHECK(g_capCount == 0);                                  // (no queued packet -> nothing to resend on)
	const ArpEntry* e = arpLookup(ipv4(10,0,2,2));
	REQUIRE(e != nullptr);
	CHECK(e->state == ARP_INCOMPLETE);
	g_fakeNow = 3000; arpTick(g_fakeNow);                    // probe 3
	g_fakeNow = 4500; arpTick(g_fakeNow);                    // exceeds MAX_PROBES -> entry freed
	CHECK(arpLookup(ipv4(10,0,2,2)) == nullptr);

	// REACHABLE entry expires to STALE after the reachable window.
	feedArp(&dev, OUR_MAC, GW_MAC, ARP_OP_REPLY, GW_MAC, ipv4(10,0,2,3), OUR_MAC, ipv4(10,0,2,15));
	const ArpEntry* r = arpLookup(ipv4(10,0,2,3));
	REQUIRE(r != nullptr);
	CHECK(r->state == ARP_REACHABLE);
	g_fakeNow += 31000; arpTick(g_fakeNow);
	CHECK(arpLookup(ipv4(10,0,2,3))->state == ARP_STALE);
	netReset(); arpReset();
}

TEST_CASE("STALE entry keeps resolving (stale MAC) and probes a refresh; a reply revives it") {
	// Regression: STALE used to be a dead end — arpResolve() returned false forever (no
	// re-request; arpTick ignores STALE), so ALL traffic to the gateway died 30s after the
	// last ARP learn. Real-world symptom: `ping google.com` -> "unknown host" (the DNS query
	// never left the box) about a minute after boot. Linux STALE semantics: keep using the
	// last known MAC, re-verify in the background.
	NetDevice dev; setup(dev);
	arpSetClock(fakeClock);
	g_fakeNow = 1000;
	// Learn the gateway (reply), then age it to STALE.
	feedArp(&dev, OUR_MAC, GW_MAC, ARP_OP_REPLY, GW_MAC, ipv4(10,0,2,2), OUR_MAC, ipv4(10,0,2,15));
	REQUIRE(arpLookup(ipv4(10,0,2,2)) != nullptr);
	g_fakeNow += 31000; arpTick(g_fakeNow);
	REQUIRE(arpLookup(ipv4(10,0,2,2))->state == ARP_STALE);

	// A STALE entry must still resolve — with the last known MAC — and emit ONE refresh probe.
	clearCap();
	uint8_t mac[6] = {0};
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == true);
	CHECK(std::memcmp(mac, GW_MAC, 6) == 0);
	CHECK(g_capCount == 1);                                  // background re-verify request
	CHECK(rd16be(g_cap + ETH_HLEN + 6) == ARP_OP_REQUEST);

	// Probes are rate-limited: an immediate second resolve does NOT send another request.
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == true);
	CHECK(g_capCount == 1);
	// ...but after PROBE_MS it re-probes again (still resolving meanwhile).
	g_fakeNow += 1500;
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == true);
	CHECK(g_capCount == 2);

	// The probe reply revives the entry to REACHABLE (normal fast path again).
	feedArp(&dev, OUR_MAC, GW_MAC, ARP_OP_REPLY, GW_MAC, ipv4(10,0,2,2), OUR_MAC, ipv4(10,0,2,15));
	CHECK(arpLookup(ipv4(10,0,2,2))->state == ARP_REACHABLE);
	clearCap();
	CHECK(arpResolve(&dev, ipv4(10,0,2,2), mac) == true);
	CHECK(g_capCount == 0);                                  // no probe when REACHABLE
	netReset(); arpReset();
}
