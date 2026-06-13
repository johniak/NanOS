/*
 * test_netproc.cpp — /proc/net/{dev,route,arp,tcp,udp,raw,snmp} renderers (FAZA 14). Populates
 * the live net-module state via the public APIs, renders each file, and checks the Linux-format
 * output (headers, address columns in network-order hex, state codes, counters).
 */
#include "doctest.h"
#include "NetProc.h"
#include "NetDevice.h"
#include "Arp.h"
#include "Ether.h"
#include "Route.h"
#include "Socket.h"
#include "Tcp.h"
#include "Unix.h"
#include "NetBuf.h"
#include "Net.h"
#include "NetStats.h"
#include <string>
#include <cstring>
#include <cstdint>

using namespace kernel;

static int noopTx(NetDevice*, NetBuf* skb) { netbufFree(skb); return 0; }

static NetDevice g_eth;   // static lifetime: the registry keeps the pointer

static void resetAll() {
	netReset(); routeReset(); arpReset(); socketReset(); tcpReset(); netStatsReset();
}

static void makeEth() {
	std::memset(&g_eth, 0, sizeof(g_eth));
	std::strcpy(g_eth.name, "eth0");
	g_eth.mac[0] = 0x52; g_eth.mac[1] = 0x54; g_eth.mac[2] = 0x00;
	g_eth.mac[3] = 0x12; g_eth.mac[4] = 0x34; g_eth.mac[5] = 0x56;
	g_eth.mtu = 1500;
	g_eth.flags = NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST;
	g_eth.ip = ipv4(10, 0, 2, 15);
	g_eth.netmask = ipv4(255, 255, 255, 0);
	g_eth.tx = noopTx;
	netRegister(&g_eth);
}

static std::string render(int (*fn)(char*, int)) {
	static char buf[4096];
	int len = fn(buf, sizeof buf);
	return std::string(buf, len);
}

TEST_CASE("/proc/net/dev: header + per-interface RX/TX counters") {
	resetAll();
	makeEth();
	g_eth.rxPackets = 10; g_eth.rxBytes = 1234; g_eth.rxErrors = 0; g_eth.rxDropped = 2;
	g_eth.txPackets = 7;  g_eth.txBytes = 891;  g_eth.txErrors = 1; g_eth.txDropped = 0;
	std::string s = render(netProcDev);
	CHECK(s.find("Inter-|") != std::string::npos);
	CHECK(s.find("Receive") != std::string::npos);
	CHECK(s.find("eth0:") != std::string::npos);
	CHECK(s.find("1234") != std::string::npos);   // rxBytes
	CHECK(s.find("891") != std::string::npos);     // txBytes
}

TEST_CASE("/proc/net/route: default + on-link routes in network-order hex") {
	resetAll();
	makeEth();
	routeAdd(ipv4(10, 0, 2, 0), ipv4(255, 255, 255, 0), 0, &g_eth, 0);  // on-link
	routeAddDefault(&g_eth, ipv4(10, 0, 2, 2));                          // default via gw
	std::string s = render(netProcRoute);
	CHECK(s.find("Iface\tDestination") != std::string::npos);
	CHECK(s.find("eth0") != std::string::npos);
	CHECK(s.find("00000000") != std::string::npos);   // default dest 0.0.0.0
	CHECK(s.find("0202000A") != std::string::npos);    // gateway 10.0.2.2 in net-order hex
	CHECK(s.find("0003") != std::string::npos);        // RTF_UP|RTF_GATEWAY on the default route
}

TEST_CASE("/proc/net/arp: resolved + incomplete entries") {
	resetAll();
	makeEth();
	uint8_t mac[6];
	// Create an INCOMPLETE entry for the gateway (sends a request via noopTx).
	arpResolve(&g_eth, ipv4(10, 0, 2, 99), mac);
	// Feed an ARP reply so a second address resolves to REACHABLE.
	arpResolve(&g_eth, ipv4(10, 0, 2, 2), mac);
	{
		NetBuf* skb = netbufAlloc();
		skb->reserve(0);
		unsigned char* a = skb->put(ARP_PLEN);
		wr16be(a + 0, ARP_HTYPE_ETH); wr16be(a + 2, ETH_P_IP);
		a[4] = 6; a[5] = 4; wr16be(a + 6, ARP_OP_REPLY);
		uint8_t gw[6] = { 0x52, 0x54, 0x00, 0x00, 0x02, 0x02 };
		std::memcpy(a + 8, gw, 6); wr32be(a + 14, ipv4(10, 0, 2, 2));
		std::memcpy(a + 18, g_eth.mac, 6); wr32be(a + 24, g_eth.ip);
		skb->dev = &g_eth;
		arpRx(skb);
	}
	std::string s = render(netProcArp);
	CHECK(s.find("IP address") != std::string::npos);
	CHECK(s.find("10.0.2.2") != std::string::npos);
	CHECK(s.find("0x2") != std::string::npos);              // ATF_COM (resolved)
	CHECK(s.find("52:54:00:00:02:02") != std::string::npos); // gateway MAC
	CHECK(s.find("10.0.2.99") != std::string::npos);          // the INCOMPLETE entry
}

TEST_CASE("/proc/net/udp: a bound DGRAM socket") {
	resetAll();
	makeEth();
	int err = 0;
	Socket* s = socketCreate(AF_INET, SOCK_DGRAM, 0, &err);
	REQUIRE(s != nullptr);
	s->bound = true; s->localIp = 0; s->localPort = 53;   // bound to *:53
	std::string out = render(netProcUdp);
	CHECK(out.find("local_address") != std::string::npos);
	CHECK(out.find("00000000:0035") != std::string::npos);  // *:53 (0x35)
}

TEST_CASE("/proc/net/unix: a listening named socket + a connected pair") {
	resetAll();
	Socket* srv = socketCreate(AF_UNIX, SOCK_STREAM, 0, nullptr);
	REQUIRE(srv);
	const char* path = "/tmp/np.sock"; unsigned pl = (unsigned) std::strlen(path) + 1;
	REQUIRE(unixBind(srv, path, pl) == 0);
	REQUIRE(unixListen(srv, 5) == 0);
	Socket *a = nullptr, *b = nullptr;
	REQUIRE(unixSocketpair(SOCK_STREAM, 0, &a, &b) == 0);

	std::string out = render(netProcUnix);
	CHECK(out.find("Num") != std::string::npos);             // header
	CHECK(out.find("RefCount") != std::string::npos);
	CHECK(out.find("/tmp/np.sock") != std::string::npos);    // the bound listener's path
	CHECK(out.find("00010000") != std::string::npos);        // SO_ACCEPTCON flag (listening)
	CHECK(out.find(" 0001 ") != std::string::npos);          // SOCK_STREAM type column
	// connected pair (a,b) renders St=03; the listener renders St=01
	CHECK(out.find(" 03 ") != std::string::npos);
	CHECK(out.find(" 01 ") != std::string::npos);
}

TEST_CASE("/proc/net/raw: a raw ICMP socket") {
	resetAll();
	makeEth();
	Socket* s = socketCreateRaw(AF_INET, SOCK_RAW, 1 /* IPPROTO_ICMP */);
	REQUIRE(s != nullptr);
	std::string out = render(netProcRaw);
	CHECK(out.find("local_address") != std::string::npos);
	CHECK(out.find(":0001") != std::string::npos);          // protocol 1 in the port column
}

TEST_CASE("/proc/net/tcp: a LISTEN socket uses Linux st numbering") {
	resetAll();
	makeEth();
	int err = 0;
	Socket* s = socketCreate(AF_INET, SOCK_STREAM, 0, &err);
	REQUIRE(s != nullptr);
	s->bound = true; s->localIp = 0; s->localPort = 80;
	REQUIRE(tcpListen(s, 5) == 0);
	std::string out = render(netProcTcp);
	CHECK(out.find("local_address rem_address") != std::string::npos);
	CHECK(out.find("0050") != std::string::npos);   // port 80
	CHECK(out.find(" 0A ") != std::string::npos);    // Linux TCP_LISTEN = 0x0A
}

TEST_CASE("/proc/net/snmp: protocol counter lines reflect g_netStats") {
	resetAll();
	g_netStats.ipInReceives = 42;
	g_netStats.ipOutRequests = 17;
	g_netStats.icmpInEchos = 3;
	g_netStats.tcpActiveOpens = 5;
	g_netStats.udpInDatagrams = 9;
	std::string s = render(netProcSnmp);
	CHECK(s.find("Ip: Forwarding") != std::string::npos);
	CHECK(s.find("Icmp:") != std::string::npos);
	CHECK(s.find("Tcp:") != std::string::npos);
	CHECK(s.find("Udp:") != std::string::npos);
	CHECK(s.find("42") != std::string::npos);   // ipInReceives
	CHECK(s.find("17") != std::string::npos);   // ipOutRequests
}
