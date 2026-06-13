// FAZA F — AF_PACKET sockets: the L2 tap + raw/cooked TX a DHCP client needs on an interface
// with no IP. Tests cover the protocol filter, cooked RX/TX framing field-by-field, delivery on
// an UNCONFIGURED device (ip == 0), and ETH_P_ALL capturing both ARP and IP.
#include "doctest.h"
#include "Packet.h"
#include "Socket.h"
#include "Ether.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include <cstdint>
#include <cstring>

using namespace kernel;

static const uint8_t OUR_MAC[6] = { 0xaa,0xbb,0xcc,0x00,0x00,0x01 };
static const uint8_t SRV_MAC[6] = { 0x52,0x55,0x0a,0x00,0x02,0x02 };
static const uint8_t BCAST[6]   = { 0xff,0xff,0xff,0xff,0xff,0xff };

static NetDevice g_dev;
static unsigned char g_cap[2048]; static int g_capLen, g_capCount;
static int captureTx(NetDevice*, NetBuf* skb) {
	g_capLen = skb->len; std::memcpy(g_cap, skb->head(), skb->len < 2048 ? skb->len : 2048);
	g_capCount++; netbufFree(skb); return 0;
}
static void clearCap() { g_capLen = 0; g_capCount = 0; }

static void setup(uint32_t ip) {
	netReset(); socketReset();    // clear stale sockets so the RX tap doesn't deliver to prior tests'
	std::memset(&g_dev, 0, sizeof g_dev);
	g_dev.name[0]='e';g_dev.name[1]='t';g_dev.name[2]='h';g_dev.name[3]='0';
	std::memcpy(g_dev.mac, OUR_MAC, 6);
	g_dev.mtu = 1500; g_dev.flags = NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST;
	g_dev.ip = ip;
	g_dev.tx = captureTx;
	netRegister(&g_dev);          // so ifindex (registry slot + 1) resolves
	ethInit();
	clearCap();
}

// Feed a raw Ethernet frame (dst, src, ethertype, payload) into the L2 RX path.
static void feedFrame(const uint8_t dst[6], const uint8_t src[6], uint16_t ethertype,
                      const unsigned char* payload, int plen) {
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	unsigned char* e = skb->put(ETH_HLEN + plen);
	std::memcpy(e, dst, 6); std::memcpy(e + 6, src, 6); wr16be(e + 12, ethertype);
	if (plen) std::memcpy(e + ETH_HLEN, payload, plen);
	skb->dev = &g_dev; ethRx(skb);
}

// ---------------------------------------------------------------------------

TEST_CASE("AF_PACKET cooked: filter by protocol; recv strips L2 + fills sockaddr_ll") {
	setup(ipv4(10,0,2,15));
	Socket* s = socketCreate(AF_PACKET, SOCK_DGRAM, hton16(ETH_P_IP), nullptr);
	REQUIRE(s);
	REQUIRE(packetBind(s, 1 /*eth0*/, 0) == 0);

	unsigned char ip[] = { 0x45,0,0,20, 0,0,0,0, 64,1,0,0, 10,0,2,2, 10,0,2,15 };
	feedFrame(OUR_MAC, SRV_MAC, ETH_P_IP, ip, sizeof ip);

	char buf[64]; int ifx, pkttype; uint16_t proto; unsigned char mac[8];
	int n = packetRecv(s, buf, sizeof buf, 0, &ifx, &proto, &pkttype, mac);
	CHECK(n == (int) sizeof ip);                       // cooked: L3 payload only (no Ethernet hdr)
	CHECK(std::memcmp(buf, ip, sizeof ip) == 0);
	CHECK(ifx == 1);
	CHECK(proto == hton16(ETH_P_IP));                  // sll_protocol reported in network order
	CHECK(pkttype == 0);                               // PACKET_HOST (dst == our MAC)
	CHECK(std::memcmp(mac, SRV_MAC, 6) == 0);          // sll_addr = source MAC

	// An ARP frame is filtered out by the ETH_P_IP socket.
	unsigned char arp[28] = {0};
	feedFrame(BCAST, SRV_MAC, ETH_P_ARP, arp, sizeof arp);
	CHECK(packetRecv(s, buf, sizeof buf, 0, &ifx, &proto, &pkttype, mac) == -SOCK_EAGAIN);
	CHECK(netbufInUse() == 0);
}

TEST_CASE("AF_PACKET cooked TX builds the Ethernet header field-by-field") {
	setup(ipv4(10,0,2,15));
	Socket* s = socketCreate(AF_PACKET, SOCK_DGRAM, hton16(ETH_P_IP), nullptr);
	REQUIRE(s);
	const char* payload = "PAYLOAD";
	int rc = packetSend(s, payload, 7, 1 /*eth0*/, hton16(ETH_P_IP), SRV_MAC);
	CHECK(rc == 7);
	REQUIRE(g_capCount == 1);
	CHECK(std::memcmp(g_cap, SRV_MAC, 6) == 0);        // dst MAC from sockaddr_ll
	CHECK(std::memcmp(g_cap + 6, OUR_MAC, 6) == 0);    // src MAC = our device
	CHECK(rd16be(g_cap + 12) == ETH_P_IP);             // ethertype from sll_protocol
	CHECK(std::memcmp(g_cap + ETH_HLEN, payload, 7) == 0);
}

TEST_CASE("AF_PACKET raw TX sends the buffer verbatim as a full frame") {
	setup(0);
	Socket* s = socketCreate(AF_PACKET, SOCK_RAW, hton16(ETH_P_ALL), nullptr);
	REQUIRE(s);
	unsigned char frame[ETH_HLEN + 4];
	std::memcpy(frame, BCAST, 6); std::memcpy(frame + 6, OUR_MAC, 6); wr16be(frame + 12, ETH_P_IP);
	frame[14]=0xde; frame[15]=0xad; frame[16]=0xbe; frame[17]=0xef;
	CHECK(packetSend(s, frame, sizeof frame, 1, 0, nullptr) == (int) sizeof frame);
	REQUIRE(g_capCount == 1);
	CHECK(g_capLen == (int) sizeof frame);
	CHECK(std::memcmp(g_cap, frame, sizeof frame) == 0);   // verbatim
}

TEST_CASE("AF_PACKET delivers on an UNCONFIGURED interface (ip == 0) — the DHCP case") {
	setup(0);                                          // no IP address at all
	Socket* s = socketCreate(AF_PACKET, SOCK_DGRAM, hton16(ETH_P_IP), nullptr);
	REQUIRE(s);
	REQUIRE(packetBind(s, 1, 0) == 0);
	unsigned char udp[] = { 0x45,0,0,28, 0,0,0,0, 64,17,0,0, 0,0,0,0, 255,255,255,255 };  // DHCP-ish
	feedFrame(BCAST, SRV_MAC, ETH_P_IP, udp, sizeof udp);   // broadcast, our IP is 0
	char buf[64]; int ifx, pkttype; uint16_t proto; unsigned char mac[8];
	int n = packetRecv(s, buf, sizeof buf, 0, &ifx, &proto, &pkttype, mac);
	CHECK(n == (int) sizeof udp);                      // delivered despite no interface IP
	CHECK(pkttype == 1);                               // PACKET_BROADCAST
}

TEST_CASE("AF_PACKET ETH_P_ALL captures both ARP and IP") {
	setup(ipv4(10,0,2,15));
	Socket* s = socketCreate(AF_PACKET, SOCK_RAW, hton16(ETH_P_ALL), nullptr);
	REQUIRE(s);
	REQUIRE(packetBind(s, 0 /*any*/, 0) == 0);
	unsigned char arp[28] = {0}; unsigned char ip[20] = {0x45};
	feedFrame(BCAST, SRV_MAC, ETH_P_ARP, arp, sizeof arp);
	feedFrame(OUR_MAC, SRV_MAC, ETH_P_IP, ip, sizeof ip);

	char buf[128]; int ifx, pkttype; uint16_t proto; unsigned char mac[8];
	int n1 = packetRecv(s, buf, sizeof buf, 0, &ifx, &proto, &pkttype, mac);
	CHECK(n1 == ETH_HLEN + (int) sizeof arp);          // raw: full frame
	CHECK(proto == hton16(ETH_P_ARP));
	int n2 = packetRecv(s, buf, sizeof buf, 0, &ifx, &proto, &pkttype, mac);
	CHECK(n2 == ETH_HLEN + (int) sizeof ip);
	CHECK(proto == hton16(ETH_P_IP));
	CHECK(netbufInUse() == 0);
}
