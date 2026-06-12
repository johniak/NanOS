// FAZA C — inbound ICMP errors (dest-unreachable / time-exceeded) are delivered to the
// matching socket, exactly like Linux WITHOUT IP_RECVERR:
//   - a CONNECTED UDP socket gets sk_err set; the next recv() returns the mapped errno once
//     (read-and-clear), then blocks normally again;
//   - an UNCONNECTED socket is NOT notified;
//   - a TCP connect() in SYN_SENT is aborted with the mapped errno (state -> CLOSED);
//   - a truncated quote is dropped without effect (hardening).
#include "doctest.h"
#include "Icmp.h"
#include "Ip.h"
#include "Route.h"
#include "Ether.h"
#include "Arp.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include "Socket.h"
#include "Udp.h"
#include "Tcp.h"
#include "Raw.h"
#include <cstdint>
#include <cstring>

using namespace kernel;

static const uint8_t OUR_MAC[6] = { 0xaa,0xbb,0xcc,0x00,0x00,0x01 };
static const uint8_t GW_MAC[6]  = { 0x52,0x55,0x0a,0x00,0x02,0x02 };

static NetDevice g_dev;
static int captureTx(NetDevice*, NetBuf* skb) { netbufFree(skb); return 0; }

static void setup() {
	netReset(); arpReset(); routeReset(); ipReset(); icmpReset(); socketReset(); tcpReset();
	std::memset(&g_dev, 0, sizeof(g_dev));
	g_dev.name[0]='e'; g_dev.name[1]='t'; g_dev.name[2]='h'; g_dev.name[3]='0';
	std::memcpy(g_dev.mac, OUR_MAC, 6);
	g_dev.mtu = 1500; g_dev.flags = NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST;
	g_dev.ip = ipv4(10,0,2,15); g_dev.netmask = ipv4(255,255,255,0); g_dev.broadcast = ipv4(10,0,2,255);
	g_dev.tx = captureTx;
	ethInit(); arpInit(); ipInit(); icmpInit(); udpInit(); rawInit(); tcpInit();
	tcpSetNewSockHook(socketCreateRaw);
	routeAddDefault(&g_dev, ipv4(10,0,2,2));
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

// Inject an ICMP error (type/code) carrying a quoted IP(proto) header [src->dst] + an 8-byte L4
// header whose first 4 bytes are sport/dport. `quoteLen` truncates the quote (hardening case).
static void feedIcmpError(uint32_t icmpSrc, uint8_t type, uint8_t code,
                          uint8_t proto, uint32_t qSrc, uint32_t qDst,
                          uint16_t sport, uint16_t dport, int quoteLen = -1) {
	unsigned char quote[28];
	std::memset(quote, 0, sizeof quote);
	quote[0]=0x45; wr16be(quote+2, 28); quote[8]=64; quote[9]=proto;
	wr32be(quote+12, qSrc); wr32be(quote+16, qDst);
	wr16be(quote+20, sport); wr16be(quote+22, dport);
	int ql = (quoteLen < 0) ? 28 : quoteLen;
	int icmpLen = 8 + ql;
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	unsigned char* h = skb->put(IP_HLEN_MIN + icmpLen);
	std::memset(h, 0, IP_HLEN_MIN + icmpLen);
	h[0]=0x45; wr16be(h+2, IP_HLEN_MIN+icmpLen); h[8]=64; h[9]=IPPROTO_ICMP;
	wr32be(h+12, icmpSrc); wr32be(h+16, g_dev.ip); wr16be(h+10, inetChecksum(h, IP_HLEN_MIN));
	unsigned char* m = h + IP_HLEN_MIN;
	m[0]=type; m[1]=code; wr16be(m+2,0); wr32be(m+4,0);
	std::memcpy(m+8, quote, ql);
	wr16be(m+2, inetChecksum(m, icmpLen));
	skb->dev = &g_dev; ipRx(skb);
}

// ---------------------------------------------------------------------------

TEST_CASE("connected UDP: ICMP port-unreachable -> next recv() returns -ECONNREFUSED once") {
	setup();
	const uint32_t peer = ipv4(10,0,2,2);
	Socket* s = socketCreate(AF_INET, SOCK_DGRAM, 0, nullptr);
	REQUIRE(socketConnect(s, peer, 5000) == 0);
	uint16_t lport = s->localPort;                 // auto-bound ephemeral source port

	feedIcmpError(peer, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH,
	              IPPROTO_UDP, g_dev.ip, peer, lport, 5000);

	char buf[64]; uint32_t si; uint16_t sp;
	CHECK(socketRecvFrom(s, buf, sizeof buf, &si, &sp, 0) == -SOCK_ECONNREFUSED); // delivered once
	CHECK(socketRecvFrom(s, buf, sizeof buf, &si, &sp, 0) == -SOCK_EAGAIN);       // then blocks again
	CHECK(netbufInUse() == 0);
}

TEST_CASE("unconnected UDP: ICMP error is NOT delivered (Linux without IP_RECVERR)") {
	setup();
	Socket* s = socketCreate(AF_INET, SOCK_DGRAM, 0, nullptr);
	REQUIRE(socketBind(s, 0, 7777) == 0);          // bound but NOT connected

	feedIcmpError(ipv4(10,0,2,2), ICMP_DEST_UNREACH, ICMP_PORT_UNREACH,
	              IPPROTO_UDP, g_dev.ip, ipv4(10,0,2,2), 7777, 5000);

	CHECK(s->soError == 0);                         // no error pending
	char buf[64]; uint32_t si; uint16_t sp;
	CHECK(socketRecvFrom(s, buf, sizeof buf, &si, &sp, 0) == -SOCK_EAGAIN);
	CHECK(netbufInUse() == 0);
}

TEST_CASE("TCP SYN_SENT: ICMP host-unreachable aborts connect with -EHOSTUNREACH") {
	setup();
	seedArp(ipv4(10,0,2,2), GW_MAC);
	const uint32_t peer = ipv4(10,0,2,2);
	Socket* s = socketCreate(AF_INET, SOCK_STREAM, 0, nullptr);
	REQUIRE(socketConnect(s, peer, 80) == 0);      // SYN sent, now SYN_SENT
	uint16_t lport = s->localPort;

	feedIcmpError(peer, ICMP_DEST_UNREACH, ICMP_HOST_UNREACH,
	              IPPROTO_TCP, g_dev.ip, peer, lport, 80);

	CHECK(s->soError == SOCK_EHOSTUNREACH);
	// No connection remains in SYN_SENT.
	TcpConnInfo info[16];
	int n = tcpSnapshot(info, 16);
	bool stillSynSent = false;
	for (int i = 0; i < n; i++)
		if (info[i].localPort == lport && info[i].state == TCP_SYN_SENT) stillSynSent = true;
	CHECK(!stillSynSent);
	CHECK(netbufInUse() == 0);
}

TEST_CASE("hardening: a truncated ICMP-error quote is dropped without effect") {
	setup();
	const uint32_t peer = ipv4(10,0,2,2);
	Socket* s = socketCreate(AF_INET, SOCK_DGRAM, 0, nullptr);
	REQUIRE(socketConnect(s, peer, 5000) == 0);
	uint16_t lport = s->localPort;

	// Quote only 4 bytes — far short of an IP header + 8; must be ignored.
	feedIcmpError(peer, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH,
	              IPPROTO_UDP, g_dev.ip, peer, lport, 5000, /*quoteLen=*/4);

	CHECK(s->soError == 0);
	CHECK(netbufInUse() == 0);
}
