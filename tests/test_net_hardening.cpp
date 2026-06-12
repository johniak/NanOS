/*
 * test_net_hardening.cpp — FAZA 14 hardening: malformed / malicious packets must never crash the
 * stack and must never leak NetBufs. Every case feeds garbage through a real RX entry point and
 * asserts (a) we return normally and (b) the NetBuf pool is back to its baseline in-use count
 * (the handler freed or bounded everything). A fragment flood must not exhaust the pool.
 */
#include "doctest.h"
#include "Ether.h"
#include "Arp.h"
#include "Ip.h"
#include "Icmp.h"
#include "Udp.h"
#include "Tcp.h"
#include "Raw.h"
#include "Route.h"
#include "Socket.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include <cstring>
#include <cstdint>

using namespace kernel;

static NetDevice g_hd;
static int hdTx(NetDevice*, NetBuf* skb) { netbufFree(skb); return 0; }

static void hardenSetup() {
	netReset(); routeReset(); arpReset(); socketReset(); tcpReset();
	std::memset(&g_hd, 0, sizeof(g_hd));
	std::strcpy(g_hd.name, "eth0");
	g_hd.mtu = 1500; g_hd.flags = NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST;
	g_hd.ip = ipv4(10, 0, 2, 15); g_hd.netmask = ipv4(255, 255, 255, 0);
	g_hd.tx = hdTx;
	netRegister(&g_hd);
	ethInit(); arpInit(); ipInit(); icmpInit(); udpInit(); tcpInit(); rawInit();
	routeAdd(ipv4(10, 0, 2, 0), ipv4(255, 255, 255, 0), 0, &g_hd, 0);
	routeAddDefault(&g_hd, ipv4(10, 0, 2, 2));
}

// Feed arbitrary bytes as an IP-layer datagram (skb->head() = the bytes) to ipRx.
static void feedIpBytes(const unsigned char* b, int n) {
	NetBuf* skb = netbufAlloc();
	skb->reserve(0);
	std::memcpy(skb->put(n), b, n);
	skb->dev = &g_hd;
	ipRx(skb);
}

// Build an IP header (+ payload) with caller-controlled fields, optionally corrupting the
// checksum, and feed it. proto/payload exercise the L4 demux under garbage.
static void feedIp(uint8_t verIhl, int totalLen, uint8_t proto, uint16_t fragField,
                   const unsigned char* payload, int plen, bool goodCsum) {
	unsigned char h[1600];
	std::memset(h, 0, sizeof(h));
	int ihl = (verIhl & 0x0f) * 4;
	h[0] = verIhl;
	h[2] = (unsigned char) (totalLen >> 8); h[3] = (unsigned char) totalLen;
	wr16be(h + 6, fragField);
	h[8] = 64; h[9] = proto;
	wr32be(h + 12, ipv4(10, 0, 2, 2));      // src (the gateway)
	wr32be(h + 16, ipv4(10, 0, 2, 15));     // dst (us)
	int hl = ihl >= 20 && ihl <= 60 ? ihl : 20;
	if (plen > 0 && payload && hl + plen <= (int) sizeof(h)) std::memcpy(h + hl, payload, plen);
	wr16be(h + 10, 0);
	if (goodCsum) wr16be(h + 10, inetChecksum(h, hl));
	else          wr16be(h + 10, 0x1234);   // deliberately wrong
	int wire = hl + (plen > 0 ? plen : 0);
	feedIpBytes(h, wire);
}

TEST_CASE("hardening: truncated / malformed IP headers are dropped without leaking") {
	hardenSetup();
	int base = netbufInUse();
	unsigned char z[64]; std::memset(z, 0, sizeof(z));
	feedIpBytes(z, 0);                                  // empty
	feedIpBytes(z, 1);                                  // 1 byte
	feedIpBytes(z, 19);                                 // shorter than the minimum header
	feedIp(0x60, 20, IPPROTO_UDP, 0, 0, 0, true);       // version 6
	feedIp(0x43, 20, IPPROTO_UDP, 0, 0, 0, true);       // ihl=3 (< 5 words)
	feedIp(0x4f, 20, IPPROTO_UDP, 0, 0, 0, true);       // ihl=15 but only 20 bytes present
	feedIp(0x45, 9999, IPPROTO_UDP, 0, 0, 0, true);     // total_len > actual length
	feedIp(0x45, 10, IPPROTO_UDP, 0, 0, 0, true);       // total_len < ihl
	feedIp(0x45, 20, IPPROTO_UDP, 0, 0, 0, false);      // bad header checksum
	CHECK(netbufInUse() == base);                       // every one freed
}

TEST_CASE("hardening: malformed ICMP/UDP/TCP payloads are dropped without leaking") {
	hardenSetup();
	int base = netbufInUse();
	unsigned char pay[40]; std::memset(pay, 0xAA, sizeof(pay));
	// ICMP: shorter than 8, then a full-length one with a bad checksum, then a bogus type.
	feedIp(0x45, 20 + 4, IPPROTO_ICMP, 0, pay, 4, true);
	feedIp(0x45, 20 + 16, IPPROTO_ICMP, 0, pay, 16, true);   // checksum over garbage -> drop
	// UDP: declared length larger than the datagram, and a runt.
	{ unsigned char u[12]; std::memset(u, 0, sizeof u); wr16be(u + 4, 9000); feedIp(0x45, 20 + 12, IPPROTO_UDP, 0, u, 12, true); }
	feedIp(0x45, 20 + 4, IPPROTO_UDP, 0, pay, 4, true);      // < UDP header
	// TCP: runt, and a segment with every flag set to a closed port (must RST, not crash/leak).
	feedIp(0x45, 20 + 4, IPPROTO_TCP, 0, pay, 4, true);
	{ unsigned char t[20]; std::memset(t, 0, sizeof t); wr16be(t + 0, 1234); wr16be(t + 2, 80);
	  t[12] = 0x50; t[13] = 0x3f /* SYN|FIN|RST|PSH|ACK|URG */; feedIp(0x45, 20 + 20, IPPROTO_TCP, 0, t, 20, true); }
	CHECK(netbufInUse() <= base + 1);   // a RST may be in flight via hdTx (freed); no growth
}

TEST_CASE("hardening: a fragment flood is bounded and never exhausts the pool") {
	hardenSetup();
	int base = netbufInUse();
	unsigned char pay[16]; std::memset(pay, 0x5A, sizeof(pay));
	// 200 distinct first-fragments (MF=1) that are never completed: reassembly must cap its
	// buffers and drop, never grabbing the whole NetBuf pool or crashing.
	for (int i = 0; i < 200; i++)
		feedIp(0x45, 20 + 16, IPPROTO_UDP, 0x2000 /* MF set, offset 0 */, pay, 16, true);
	CHECK(netbufInUse() < 96);          // POOL_N — never exhausted
	ipReasmTick(100000000u);            // expire everything
	CHECK(netbufInUse() == base);       // all reassembly buffers reclaimed
}

TEST_CASE("hardening: malformed Ethernet / ARP frames are dropped without leaking") {
	hardenSetup();
	int base = netbufInUse();
	// Runt Ethernet frame (smaller than the 14-byte header).
	{ NetBuf* s = netbufAlloc(); s->reserve(0); std::memset(s->put(8), 0, 8); s->dev = &g_hd; ethRx(s); }
	// Unknown ethertype.
	{ NetBuf* s = netbufAlloc(); s->reserve(0); unsigned char* e = s->put(ETH_HLEN + 4);
	  std::memset(e, 0, ETH_HLEN + 4); wr16be(e + 12, 0x9999); s->dev = &g_hd; ethRx(s); }
	// Truncated ARP (ethertype ARP but body shorter than ARP_PLEN).
	{ NetBuf* s = netbufAlloc(); s->reserve(0); unsigned char* e = s->put(ETH_HLEN + 10);
	  std::memset(e, 0, ETH_HLEN + 10); wr16be(e + 12, ETH_P_ARP); s->dev = &g_hd; ethRx(s); }
	// ARP with a non-Ethernet/IP htype/ptype and a bogus opcode.
	{ NetBuf* s = netbufAlloc(); s->reserve(0); unsigned char* e = s->put(ETH_HLEN + ARP_PLEN);
	  std::memset(e, 0, ETH_HLEN + ARP_PLEN); wr16be(e + 12, ETH_P_ARP);
	  unsigned char* a = e + ETH_HLEN; wr16be(a + 0, 0xFFFF); wr16be(a + 2, 0x1234);
	  a[4] = 6; a[5] = 4; wr16be(a + 6, 0x4242); s->dev = &g_hd; ethRx(s); }
	CHECK(netbufInUse() == base);
}
