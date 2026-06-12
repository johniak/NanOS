#include "doctest.h"
#include "Icmp.h"
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
	g_capLen = skb->len; std::memcpy(g_cap, skb->head(), skb->len < 2048 ? skb->len : 2048);
	g_capCount++; netbufFree(skb); return 0;
}
static void clearCap() { g_capLen = 0; g_capCount = 0; std::memset(g_cap, 0, sizeof(g_cap)); }

static NetDevice g_dev;
static void setup() {
	netReset(); arpReset(); routeReset(); ipReset(); icmpReset();
	std::memset(&g_dev, 0, sizeof(g_dev));
	g_dev.name[0]='e'; g_dev.name[1]='t'; g_dev.name[2]='h'; g_dev.name[3]='0';
	std::memcpy(g_dev.mac, OUR_MAC, 6);
	g_dev.mtu = 1500; g_dev.flags = NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST;
	g_dev.ip = ipv4(10,0,2,15); g_dev.netmask = ipv4(255,255,255,0); g_dev.broadcast = ipv4(10,0,2,255);
	g_dev.tx = captureTx;
	ethInit(); arpInit(); ipInit(); icmpInit();
	routeAddDefault(&g_dev, ipv4(10,0,2,2));
	clearCap();
}

static void seedArp(uint32_t ip, const uint8_t mac[6]) {
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	unsigned char* e = skb->put(ETH_HLEN + ARP_PLEN);
	std::memcpy(e, OUR_MAC, 6); std::memcpy(e+6, mac, 6); wr16be(e+12, ETH_P_ARP);
	unsigned char* a = e + ETH_HLEN;
	wr16be(a+0, ARP_HTYPE_ETH); wr16be(a+2, ETH_P_IP); a[4]=6; a[5]=4; wr16be(a+6, ARP_OP_REPLY);
	std::memcpy(a+8, mac, 6); wr32be(a+14, ip); std::memcpy(a+18, OUR_MAC, 6); wr32be(a+24, g_dev.ip);
	skb->dev = &g_dev; ethRx(skb);
}

// Feed an IP packet carrying an ICMP message to ipRx (which demuxes to icmpRx).
static void feedIcmp(uint32_t src, uint8_t type, uint8_t code, uint16_t id, uint16_t seq,
                     const unsigned char* data, int dlen) {
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	int icmpLen = 8 + dlen;
	unsigned char* h = skb->put(IP_HLEN_MIN + icmpLen);
	h[0]=0x45; wr16be(h+2, IP_HLEN_MIN+icmpLen); wr16be(h+4,0x1111); wr16be(h+6,0); h[8]=64; h[9]=IPPROTO_ICMP;
	wr32be(h+12, src); wr32be(h+16, g_dev.ip); wr16be(h+10,0); wr16be(h+10, inetChecksum(h, IP_HLEN_MIN));
	unsigned char* m = h + IP_HLEN_MIN;
	m[0]=type; m[1]=code; wr16be(m+2,0); wr16be(m+4,id); wr16be(m+6,seq);
	if (dlen) std::memcpy(m+8, data, dlen);
	wr16be(m+2, inetChecksum(m, icmpLen));
	skb->dev = &g_dev; ipRx(skb);
}

// ---------------------------------------------------------------------------

TEST_CASE("ICMP echo request -> byte-exact echo reply on the wire") {
	setup();
	seedArp(ipv4(10,0,2,2), GW_MAC);   // reply goes back to the (on-link) sender
	clearCap();
	unsigned char payload[32]; for (int i=0;i<32;i++) payload[i]=(unsigned char)(0xC0+i);
	feedIcmp(ipv4(10,0,2,2), ICMP_ECHO_REQUEST, 0, 0xABCD, 7, payload, 32);

	REQUIRE(g_capCount == 1);
	const unsigned char* h = g_cap + ETH_HLEN;          // IP header
	CHECK(h[9] == IPPROTO_ICMP);
	CHECK(rd32be(h+12) == ipv4(10,0,2,15));             // src = us
	CHECK(rd32be(h+16) == ipv4(10,0,2,2));              // dst = original sender
	CHECK(inetChecksum(h, IP_HLEN_MIN) == 0);
	const unsigned char* m = h + IP_HLEN_MIN;           // ICMP
	CHECK(m[0] == ICMP_ECHO_REPLY);
	CHECK(m[1] == 0);
	CHECK(rd16be(m+4) == 0xABCD);                       // id preserved
	CHECK(rd16be(m+6) == 7);                            // seq preserved
	CHECK(std::memcmp(m+8, payload, 32) == 0);          // data echoed
	CHECK(inetChecksum(m, 8 + 32) == 0);                // ICMP checksum valid
}

static uint32_t g_erSrc; static uint16_t g_erId, g_erSeq; static int g_erCount;
static void echoReplyCb(uint32_t src, uint16_t id, uint16_t seq) { g_erSrc=src; g_erId=id; g_erSeq=seq; g_erCount++; }

TEST_CASE("ICMP echo reply -> echo-reply handler (kernel ping result)") {
	setup();
	g_erCount = 0;
	icmpSetEchoReplyHandler(echoReplyCb);
	feedIcmp(ipv4(10,0,2,2), ICMP_ECHO_REPLY, 0, 0x4242, 99, nullptr, 0);
	REQUIRE(g_erCount == 1);
	CHECK(g_erSrc == ipv4(10,0,2,2));
	CHECK(g_erId == 0x4242);
	CHECK(g_erSeq == 99);
	CHECK(g_capCount == 0);   // a reply is not auto-answered
}

static int g_rawCount;
static void rawCb(NetBuf* skb) { g_rawCount++; netbufFree(skb); }

TEST_CASE("raw handler receives ICMP; takes precedence over the echo-reply handler") {
	setup();
	g_rawCount = 0; g_erCount = 0;
	icmpSetRawHandler(rawCb);
	icmpSetEchoReplyHandler(echoReplyCb);
	feedIcmp(ipv4(10,0,2,2), ICMP_ECHO_REPLY, 0, 1, 1, nullptr, 0);
	CHECK(g_rawCount == 1);
	CHECK(g_erCount == 0);    // raw installed -> echo-reply handler not used
	CHECK(netbufInUse() == 0);
}

TEST_CASE("ICMP bad checksum is dropped") {
	setup();
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	unsigned char* h = skb->put(IP_HLEN_MIN + 8);
	std::memset(h, 0, IP_HLEN_MIN + 8);
	h[0]=0x45; wr16be(h+2, IP_HLEN_MIN+8); h[8]=64; h[9]=IPPROTO_ICMP;
	wr32be(h+12, ipv4(10,0,2,2)); wr32be(h+16, g_dev.ip); wr16be(h+10,0); wr16be(h+10, inetChecksum(h, IP_HLEN_MIN));
	unsigned char* m = h + IP_HLEN_MIN;
	m[0]=ICMP_ECHO_REQUEST; m[1]=0; wr16be(m+2,0xDEAD);    // deliberately wrong checksum
	skb->dev = &g_dev; ipRx(skb);
	CHECK(g_capCount == 0);
	CHECK(netbufInUse() == 0);
}

TEST_CASE("icmpSendEcho emits a valid echo request") {
	setup();
	seedArp(ipv4(10,0,2,2), GW_MAC);
	clearCap();
	unsigned char data[16]; for (int i=0;i<16;i++) data[i]=(unsigned char)i;
	CHECK(icmpSendEcho(ipv4(212,77,98,9), 0x1234, 5, data, 16) == 0);
	REQUIRE(g_capCount == 1);
	const unsigned char* h = g_cap + ETH_HLEN;
	CHECK(h[9] == IPPROTO_ICMP);
	CHECK(rd32be(h+16) == ipv4(212,77,98,9));
	const unsigned char* m = h + IP_HLEN_MIN;
	CHECK(m[0] == ICMP_ECHO_REQUEST);
	CHECK(rd16be(m+4) == 0x1234);
	CHECK(rd16be(m+6) == 5);
	CHECK(inetChecksum(m, 8 + 16) == 0);
}

TEST_CASE("icmpSendError quotes the offending IP header + 8 bytes (port unreachable)") {
	setup();
	seedArp(ipv4(10,0,2,2), GW_MAC);
	// Build a received UDP-ish IP packet and demux far enough that l3/l4 are set, then trigger
	// an error about it (as UDP would when no socket is bound).
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	unsigned char* h = skb->put(IP_HLEN_MIN + 8 + 4);
	std::memset(h, 0, IP_HLEN_MIN + 8 + 4);   // put() doesn't zero; clear flags/frag/id etc.
	h[0]=0x45; wr16be(h+2, IP_HLEN_MIN+8+4); h[8]=64; h[9]=IPPROTO_UDP;
	wr32be(h+12, ipv4(10,0,2,2)); wr32be(h+16, g_dev.ip); wr16be(h+10,0); wr16be(h+10, inetChecksum(h, IP_HLEN_MIN));
	// Hand it through a stub UDP handler that calls icmpSendError.
	static NetBuf* captured = nullptr;
	ipSetHandler(IPPROTO_UDP, [](NetBuf* s){ captured = s; });
	skb->dev = &g_dev; ipRx(skb);
	REQUIRE(captured != nullptr);
	clearCap();
	icmpSendError(captured, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH);
	REQUIRE(g_capCount == 1);
	const unsigned char* H = g_cap + ETH_HLEN;
	const unsigned char* M = H + IP_HLEN_MIN;
	CHECK(M[0] == ICMP_DEST_UNREACH);
	CHECK(M[1] == ICMP_PORT_UNREACH);
	CHECK(inetChecksum(M, rd16be(H+2) - IP_HLEN_MIN) == 0);
	// The quoted region begins with the offending IP header (version/IHL 0x45, proto UDP).
	CHECK(M[8] == 0x45);
	CHECK(M[8 + 9] == IPPROTO_UDP);
	netbufFree(captured);
}
