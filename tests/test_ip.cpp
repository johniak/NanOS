#include "doctest.h"
#include "Ip.h"
#include "Route.h"
#include "Ether.h"
#include "Arp.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include <cstdint>
#include <cstring>

using namespace kernel;

static const uint8_t OUR_MAC[6] = { 0xaa,0xbb,0xcc,0x00,0x00,0x01 };
static const uint8_t GW_MAC[6]  = { 0x52,0x55,0x0a,0x00,0x02,0x02 };

static unsigned char g_cap[2048];
static int g_capLen, g_capCount;
static int captureTx(NetDevice*, NetBuf* skb) {
	g_capLen = skb->len;
	std::memcpy(g_cap, skb->head(), skb->len < 2048 ? skb->len : 2048);
	g_capCount++; netbufFree(skb); return 0;
}
static void clearCap() { g_capLen = 0; g_capCount = 0; std::memset(g_cap, 0, sizeof(g_cap)); }

static int g_protoSeen, g_protoLen; static uint32_t g_protoSrc, g_protoDst; static uint8_t g_protoNum;
static void captureProto(NetBuf* skb) {
	g_protoSeen++; g_protoLen = skb->len;
	g_protoSrc = skb->saddr; g_protoDst = skb->daddr; g_protoNum = skb->ipproto;
	netbufFree(skb);
}

static NetDevice g_dev;
static void setup() {
	netReset(); arpReset(); routeReset(); ipReset();
	std::memset(&g_dev, 0, sizeof(g_dev));
	g_dev.name[0]='e'; g_dev.name[1]='t'; g_dev.name[2]='h'; g_dev.name[3]='0';
	std::memcpy(g_dev.mac, OUR_MAC, 6);
	g_dev.mtu = 1500; g_dev.flags = NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST;
	g_dev.ip = ipv4(10,0,2,15); g_dev.netmask = ipv4(255,255,255,0);
	g_dev.broadcast = ipv4(10,0,2,255);
	g_dev.tx = captureTx;
	ethInit(); arpInit(); ipInit();
	clearCap();
	g_protoSeen = 0; g_protoLen = 0; g_protoSrc = g_protoDst = 0; g_protoNum = 0;
}

// Build a complete IP packet (header + payload) into a NetBuf and hand it to ipRx.
static void feedIp(uint32_t src, uint32_t dst, uint8_t proto, const unsigned char* payload, int plen,
                   uint16_t id = 0x1234, uint16_t flagsFrag = 0) {
	NetBuf* skb = netbufAlloc();
	skb->reserve(0);
	unsigned char* h = skb->put(IP_HLEN_MIN + plen);
	h[0]=0x45; h[1]=0; wr16be(h+2, IP_HLEN_MIN+plen); wr16be(h+4, id);
	wr16be(h+6, flagsFrag); h[8]=64; h[9]=proto; wr16be(h+10,0);
	wr32be(h+12, src); wr32be(h+16, dst);
	wr16be(h+10, inetChecksum(h, IP_HLEN_MIN));
	if (plen) std::memcpy(h + IP_HLEN_MIN, payload, plen);
	skb->dev = &g_dev;
	ipRx(skb);
}

// Seed the ARP cache so arpResolve(ip) hits: inject a reply for ip from mac.
static void seedArp(uint32_t ip, const uint8_t mac[6]) {
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	unsigned char* e = skb->put(ETH_HLEN + ARP_PLEN);
	std::memcpy(e, OUR_MAC, 6); std::memcpy(e+6, mac, 6); wr16be(e+12, ETH_P_ARP);
	unsigned char* a = e + ETH_HLEN;
	wr16be(a+0, ARP_HTYPE_ETH); wr16be(a+2, ETH_P_IP); a[4]=6; a[5]=4; wr16be(a+6, ARP_OP_REPLY);
	std::memcpy(a+8, mac, 6); wr32be(a+14, ip); std::memcpy(a+18, OUR_MAC, 6); wr32be(a+24, g_dev.ip);
	skb->dev = &g_dev;
	ethRx(skb);
}

// ---------------- routing ----------------

TEST_CASE("route: longest-prefix beats default; on-link vs gateway next hop") {
	setup();
	routeReset();
	routeAddDefault(&g_dev, ipv4(10,0,2,2));   // adds 10.0.2.0/24 on-link + 0.0.0.0/0 via gw
	NetDevice* dev; uint32_t nh;
	// On-link host: next hop is the host itself.
	REQUIRE(routeLookup(ipv4(10,0,2,9), &dev, &nh));
	CHECK(dev == &g_dev);
	CHECK(nh == ipv4(10,0,2,9));
	// Off-link host: next hop is the gateway.
	REQUIRE(routeLookup(ipv4(212,77,98,9), &dev, &nh));
	CHECK(nh == ipv4(10,0,2,2));
	// A more specific route wins over the subnet route.
	routeAdd(ipv4(10,0,2,8), ipv4(255,255,255,248), ipv4(10,0,2,3), &g_dev, 0);
	REQUIRE(routeLookup(ipv4(10,0,2,9), &dev, &nh));
	CHECK(nh == ipv4(10,0,2,3));    // /29 is longer than /24
	routeReset();
	CHECK(routeCount() == 0);
	CHECK(routeLookup(ipv4(1,1,1,1), &dev, &nh) == false);
}

TEST_CASE("route: del, replace, metric tie-break, iteration, full table") {
	setup(); routeReset();
	routeAdd(ipv4(10,0,0,0), ipv4(255,0,0,0), ipv4(10,0,2,2), &g_dev, 5);
	routeAdd(ipv4(10,0,0,0), ipv4(255,0,0,0), ipv4(10,0,2,3), &g_dev, 1);   // same prefix -> replace
	CHECK(routeCount() == 1);
	NetDevice* dev; uint32_t nh;
	REQUIRE(routeLookup(ipv4(10,5,5,5), &dev, &nh));
	CHECK(nh == ipv4(10,0,2,3));                          // replaced gw

	// Metric tie-break: two equal-length prefixes, lower metric wins.
	routeReset();
	routeAdd(ipv4(192,168,0,0), ipv4(255,255,0,0), ipv4(1,1,1,1), &g_dev, 10);
	routeAdd(ipv4(192,168,0,0), ipv4(255,255,0,0), ipv4(2,2,2,2), &g_dev, 3);  // replaces (same dest/mask)
	REQUIRE(routeLookup(ipv4(192,168,1,1), &dev, &nh));
	CHECK(nh == ipv4(2,2,2,2));

	// routeByIndex iterates used entries.
	routeReset();
	routeAdd(ipv4(10,0,0,0), ipv4(255,0,0,0), 0, &g_dev, 0);
	routeAdd(0, 0, ipv4(10,0,0,1), &g_dev, 0);
	CHECK(routeCount() == 2);
	CHECK(routeByIndex(0) != nullptr);
	CHECK(routeByIndex(1) != nullptr);
	CHECK(routeByIndex(2) == nullptr);

	// routeDel removes.
	routeDel(0, 0);
	CHECK(routeCount() == 1);
	CHECK(routeLookup(ipv4(8,8,8,8), &dev, &nh) == false);   // default gone

	// routeAddDefault ignores a null device.
	routeAddDefault(nullptr, ipv4(1,2,3,4));
	routeReset();
}

// ---------------- IP input demux ----------------

TEST_CASE("ipRx: valid packet demuxes to the proto handler with src/dst/proto") {
	setup();
	ipSetHandler(IPPROTO_UDP, captureProto);
	unsigned char pay[20]; for (int i=0;i<20;i++) pay[i]=(unsigned char)(0x30+i);
	feedIp(ipv4(212,77,98,9), ipv4(10,0,2,15), IPPROTO_UDP, pay, 20);
	REQUIRE(g_protoSeen == 1);
	CHECK(g_protoSrc == ipv4(212,77,98,9));
	CHECK(g_protoDst == ipv4(10,0,2,15));
	CHECK(g_protoNum == IPPROTO_UDP);
	CHECK(g_protoLen == 20);                    // IP header stripped, payload handed up
}

TEST_CASE("ipRx: bad checksum / runt / not-for-us are dropped") {
	setup();
	ipSetHandler(IPPROTO_UDP, captureProto);
	// Corrupt checksum.
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	unsigned char* h = skb->put(IP_HLEN_MIN + 4);
	h[0]=0x45; wr16be(h+2, IP_HLEN_MIN+4); h[8]=64; h[9]=IPPROTO_UDP; wr32be(h+12, ipv4(1,2,3,4));
	wr32be(h+16, ipv4(10,0,2,15)); wr16be(h+10, 0x0000);   // wrong checksum
	skb->dev = &g_dev; ipRx(skb);
	CHECK(g_protoSeen == 0);
	// Runt.
	NetBuf* r = netbufAlloc(); r->reserve(0); r->put(10); r->dev = &g_dev; ipRx(r);
	CHECK(g_protoSeen == 0);
	// Not for us (dst is some other host).
	unsigned char pay[4] = {1,2,3,4};
	feedIp(ipv4(1,2,3,4), ipv4(8,8,8,8), IPPROTO_UDP, pay, 4);
	CHECK(g_protoSeen == 0);
	CHECK(netbufInUse() == 0);
}

TEST_CASE("ipRx: accepts limited broadcast") {
	setup();
	ipSetHandler(IPPROTO_UDP, captureProto);
	unsigned char pay[4] = {9,9,9,9};
	feedIp(ipv4(10,0,2,2), 0xFFFFFFFFu, IPPROTO_UDP, pay, 4);
	CHECK(g_protoSeen == 1);
}

// ---------------- IP output ----------------

TEST_CASE("ipOutput: builds a valid header, routes via gateway, sends as an IP frame") {
	setup();
	routeAddDefault(&g_dev, ipv4(10,0,2,2));
	seedArp(ipv4(10,0,2,2), GW_MAC);            // so arpResolve(gw) hits immediately
	clearCap();
	NetBuf* skb = netbufAlloc(); skb->reserve(NET_HEADROOM);
	unsigned char* p = skb->put(30); for (int i=0;i<30;i++) p[i]=(unsigned char)(0x41+i);
	CHECK(ipOutput(ipv4(212,77,98,9), IPPROTO_ICMP, skb) == 0);
	REQUIRE(g_capCount == 1);
	// Ethernet to the gateway MAC, type IP.
	CHECK(std::memcmp(g_cap, GW_MAC, 6) == 0);
	CHECK(std::memcmp(g_cap+6, OUR_MAC, 6) == 0);
	CHECK(rd16be(g_cap+12) == ETH_P_IP);
	const unsigned char* h = g_cap + ETH_HLEN;
	CHECK(h[0] == 0x45);
	CHECK(rd16be(h+2) == (uint16_t)(IP_HLEN_MIN + 30));
	CHECK(h[8] == IP_DEFAULT_TTL);
	CHECK(h[9] == IPPROTO_ICMP);
	CHECK(rd32be(h+12) == ipv4(10,0,2,15));    // src = our IP
	CHECK(rd32be(h+16) == ipv4(212,77,98,9));  // dst
	CHECK(inetChecksum(h, IP_HLEN_MIN) == 0);  // header checksum valid
}

TEST_CASE("ipOutput: no route -> drops, no leak") {
	setup();
	NetBuf* skb = netbufAlloc(); skb->reserve(NET_HEADROOM); skb->put(8);
	CHECK(ipOutput(ipv4(9,9,9,9), IPPROTO_UDP, skb) == -1);
	CHECK(netbufInUse() == 0);
}

// ---------------- fragmentation / reassembly ----------------

TEST_CASE("reassembly: in-order fragments rebuild the datagram") {
	setup();
	ipSetHandler(IPPROTO_UDP, captureProto);
	unsigned char a[16]; for (int i=0;i<16;i++) a[i]=(unsigned char)(0xA0+i);
	unsigned char b[8];  for (int i=0;i<8;i++)  b[i]=(unsigned char)(0xB0+i);
	feedIp(ipv4(8,8,8,8), ipv4(10,0,2,15), IPPROTO_UDP, a, 16, 0x7777, IP_FLAG_MF);       // off 0, MF
	CHECK(g_protoSeen == 0);                                                              // incomplete
	feedIp(ipv4(8,8,8,8), ipv4(10,0,2,15), IPPROTO_UDP, b, 8, 0x7777, (uint16_t)(16/8));  // off 16, last
	REQUIRE(g_protoSeen == 1);
	CHECK(g_protoLen == 24);                                                              // 16 + 8 reassembled
}

TEST_CASE("reassembly: out-of-order fragments still rebuild") {
	setup();
	ipSetHandler(IPPROTO_UDP, captureProto);
	unsigned char a[16]; for (int i=0;i<16;i++) a[i]=(unsigned char)(0xA0+i);
	unsigned char b[8];  for (int i=0;i<8;i++)  b[i]=(unsigned char)(0xB0+i);
	feedIp(ipv4(8,8,8,8), ipv4(10,0,2,15), IPPROTO_UDP, b, 8, 0x8888, (uint16_t)(16/8));  // last first
	CHECK(g_protoSeen == 0);
	feedIp(ipv4(8,8,8,8), ipv4(10,0,2,15), IPPROTO_UDP, a, 16, 0x8888, IP_FLAG_MF);       // then off 0
	REQUIRE(g_protoSeen == 1);
	CHECK(g_protoLen == 24);
}

TEST_CASE("reassembly: incomplete datagram expires on tick") {
	setup();
	ipSetHandler(IPPROTO_UDP, captureProto);
	static unsigned clk; clk = 1000;
	ipReasmSetClock([]() -> unsigned { return clk; });
	unsigned char a[16]; for (int i=0;i<16;i++) a[i]=(unsigned char)i;
	feedIp(ipv4(8,8,8,8), ipv4(10,0,2,15), IPPROTO_UDP, a, 16, 0x9999, IP_FLAG_MF);   // first frag only
	clk = 1000 + 31000; ipReasmTick(clk);                                            // > 30 s -> expire
	// The completing fragment now finds no context -> starts fresh, stays incomplete, no delivery.
	unsigned char b[8]; for (int i=0;i<8;i++) b[i]=(unsigned char)i;
	feedIp(ipv4(8,8,8,8), ipv4(10,0,2,15), IPPROTO_UDP, b, 8, 0x9999, (uint16_t)(16/8));
	CHECK(g_protoSeen == 0);
	netReset(); arpReset(); routeReset(); ipReset();
}

TEST_CASE("TX fragmentation: a payload over the MTU splits into correct fragments") {
	setup();
	g_dev.mtu = 600;                            // small MTU to force fragmentation
	routeAddDefault(&g_dev, ipv4(10,0,2,2));
	seedArp(ipv4(10,0,2,2), GW_MAC);
	clearCap();
	// Collect every emitted fragment.
	static unsigned char frags[8][2048]; static int nfrag;
	nfrag = 0;
	g_dev.tx = [](NetDevice*, NetBuf* skb) -> int {
		if (nfrag < 8) { std::memcpy(frags[nfrag], skb->head(), skb->len); nfrag++; }
		netbufFree(skb); return 0;
	};
	int total = 1400;
	NetBuf* skb = netbufAlloc(); skb->reserve(NET_HEADROOM);
	unsigned char* p = skb->put(total); for (int i=0;i<total;i++) p[i]=(unsigned char)(i & 0xff);
	ipOutput(ipv4(212,77,98,9), IPPROTO_UDP, skb);
	// MTU 600 -> max IP payload (600-20) & ~7 = 576 per fragment; 1400 -> 576+576+248 = 3 fragments.
	REQUIRE(nfrag == 3);
	int reasm = 0;
	for (int f = 0; f < nfrag; f++) {
		const unsigned char* h = frags[f] + ETH_HLEN;
		CHECK(h[0] == 0x45);
		CHECK(inetChecksum(h, IP_HLEN_MIN) == 0);            // each fragment header valid
		uint16_t ff = rd16be(h + 6);
		int off = (ff & IP_FRAG_MASK) * 8;
		bool mf = (ff & IP_FLAG_MF) != 0;
		int plen = rd16be(h + 2) - IP_HLEN_MIN;
		CHECK(off == reasm);                                 // contiguous offsets
		CHECK(mf == (f != nfrag - 1));                       // MF set on all but the last
		reasm += plen;
	}
	CHECK(reasm == total);                                   // fragments cover the whole payload
	netReset(); arpReset(); routeReset(); ipReset();
}
