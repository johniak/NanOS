// FAZA 9: the socket syscall layer over the fd table (Syscalls::sock*). Drives the same code the
// int-0x80 dispatch calls, plus the fd integration (read/write/close/dup route to the socket).
#include "doctest.h"
#include "Syscall.h"
#include "Vfs.h"
#include "Ext2Filesystem.h"
#include "RamBlockDevice.h"
#include "Socket.h"
#include "Tcp.h"
#include "Udp.h"
#include "Raw.h"
#include "Ip.h"
#include "Route.h"
#include "Ether.h"
#include "Arp.h"
#include "Icmp.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <initializer_list>

using namespace kernel;

static int sysSink(const char*, unsigned n) { return (int) n; }
static Vfs* mountFixture2() {
	FILE* f = fopen("tests/fixtures/ext2.img", "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	char* buf = (char*) malloc(sz);
	REQUIRE(fread(buf, 1, sz, f) == (size_t) sz);
	fclose(f);
	RamBlockDevice* dev = new RamBlockDevice("d", buf, (unsigned) sz);
	Vfs* vfs = new Vfs();
	static Ext2FileSystemType t; vfs->registerType(&t);
	REQUIRE(vfs->mount("/", "ext2", dev, 0) == 0);
	return vfs;
}

static const uint8_t OUR_MAC[6] = { 0xaa,0xbb,0xcc,0x00,0x00,0x01 };
static const uint8_t GW_MAC[6]  = { 0x52,0x55,0x0a,0x00,0x02,0x02 };
static unsigned char g_cap[2048]; static int g_capLen, g_capCount;
static int captureTx(NetDevice*, NetBuf* skb) { g_capLen=skb->len; std::memcpy(g_cap,skb->head(),skb->len<2048?skb->len:2048); g_capCount++; netbufFree(skb); return 0; }
static NetDevice g_dev;

static void netSetup() {
	netReset(); arpReset(); routeReset(); ipReset(); icmpReset(); socketReset(); tcpReset();
	std::memset(&g_dev,0,sizeof(g_dev));
	g_dev.name[0]='e';g_dev.name[1]='t';g_dev.name[2]='h';g_dev.name[3]='0';
	std::memcpy(g_dev.mac,OUR_MAC,6);
	g_dev.mtu=1500; g_dev.flags=NETIF_UP|NETIF_RUNNING|NETIF_BROADCAST;
	g_dev.ip=ipv4(10,0,2,15); g_dev.netmask=ipv4(255,255,255,0); g_dev.broadcast=ipv4(10,0,2,255);
	g_dev.tx=captureTx; netRegister(&g_dev);
	ethInit(); arpInit(); ipInit(); icmpInit(); udpInit(); rawInit(); tcpInit();
	tcpSetNewSockHook(socketCreateRaw);
	routeAddDefault(&g_dev, ipv4(10,0,2,2));
	// seed ARP for gw + peers used below
	for (uint32_t ip : { ipv4(10,0,2,2), ipv4(212,77,98,9) }) {
		NetBuf* s=netbufAlloc(); s->reserve(0);
		unsigned char* e=s->put(ETH_HLEN+ARP_PLEN);
		std::memcpy(e,OUR_MAC,6); std::memcpy(e+6,GW_MAC,6); wr16be(e+12,ETH_P_ARP);
		unsigned char* a=e+ETH_HLEN; wr16be(a,ARP_HTYPE_ETH); wr16be(a+2,ETH_P_IP); a[4]=6;a[5]=4;
		wr16be(a+6,ARP_OP_REPLY); std::memcpy(a+8,GW_MAC,6); wr32be(a+14,ip); std::memcpy(a+18,OUR_MAC,6); wr32be(a+24,g_dev.ip);
		s->dev=&g_dev; ethRx(s);
	}
	g_capLen=0; g_capCount=0;
}

// Build a Linux sockaddr_in (16 bytes) with the given host-order ip/port.
static void mkaddr(unsigned char sa[16], uint32_t ip, uint16_t port) {
	std::memset(sa,0,16); sa[0]=2; sa[1]=0; wr16be(sa+2,port); wr32be(sa+4,ip);
}
static void feedUdp(uint32_t src, uint16_t sport, uint16_t dport, const unsigned char* d, int dl) {
	NetBuf* skb=netbufAlloc(); skb->reserve(0);
	int ul=8+dl, tot=IP_HLEN_MIN+ul; unsigned char* h=skb->put(tot); std::memset(h,0,tot);
	h[0]=0x45; wr16be(h+2,tot); h[8]=64; h[9]=IPPROTO_UDP; wr32be(h+12,src); wr32be(h+16,g_dev.ip);
	wr16be(h+10,inetChecksum(h,IP_HLEN_MIN));
	unsigned char* u=h+IP_HLEN_MIN; wr16be(u,sport); wr16be(u+2,dport); wr16be(u+4,ul);
	if(dl) std::memcpy(u+8,d,dl);
	unsigned char ph[12]; wr32be(ph,src); wr32be(ph+4,g_dev.ip); ph[8]=0; ph[9]=IPPROTO_UDP; wr16be(ph+10,ul);
	uint32_t sum=inetChecksumAccum(ph,12,0); sum=inetChecksumAccum(u,ul,sum); uint16_t c=inetChecksumFinish(sum);
	wr16be(u+6, c?c:0xFFFF); skb->dev=&g_dev; ipRx(skb);
}

// ---------------------------------------------------------------------------

TEST_CASE("socket()/bind()/getsockopt over the fd table") {
	netSetup();
	Syscalls sc(mountFixture2(), sysSink);
	int fd = sc.sockSocket(AF_INET, SOCK_DGRAM, 0);
	CHECK(fd >= 3);
	CHECK(sc.isSocketFd(fd) == true);
	unsigned char sa[16]; mkaddr(sa, 0, 9999);
	CHECK(sc.sockBind(fd, sa, 16) == 0);
	int v; unsigned l = sizeof(v);
	CHECK(sc.sockGetsockopt(fd, SOL_SOCKET, SO_TYPE, &v, &l) == 0);
	CHECK(v == SOCK_DGRAM);
	CHECK(sc.close(fd) == 0);
	CHECK(sc.isSocketFd(fd) == false);
}

TEST_CASE("UDP recvfrom via the fd: read() returns the datagram + source sockaddr") {
	netSetup();
	Syscalls sc(mountFixture2(), sysSink);
	int fd = sc.sockSocket(AF_INET, SOCK_DGRAM, 0);
	unsigned char sa[16]; mkaddr(sa, 0, 5353);
	sc.sockBind(fd, sa, 16);
	CHECK(sc.read(fd, sa, 4) == -EAGAIN);              // empty -> would block
	unsigned char payload[8] = { 'D','N','S','r','e','p','l','y' };
	feedUdp(ipv4(8,8,8,8), 53, 5353, payload, 8);
	unsigned char buf[64], from[16]; unsigned fl = 16;
	int n = sc.sockRecvfrom(fd, buf, sizeof(buf), 0, from, &fl);
	CHECK(n == 8);
	CHECK(std::memcmp(buf, payload, 8) == 0);
	CHECK(from[0] == 2);                               // AF_INET
	CHECK(rd16be(from + 2) == 53);                     // source port
	CHECK(rd32be(from + 4) == ipv4(8,8,8,8));          // source ip
	sc.close(fd);
}

TEST_CASE("UDP connect + write() sends to the connected peer") {
	netSetup();
	Syscalls sc(mountFixture2(), sysSink);
	int fd = sc.sockSocket(AF_INET, SOCK_DGRAM, 0);
	unsigned char sa[16]; mkaddr(sa, ipv4(212,77,98,9), 53);
	CHECK(sc.sockConnect(fd, sa, 16) == 0);            // UDP connect is immediate
	g_capLen=0; g_capCount=0;
	CHECK(sc.write(fd, "qq", 2) == 2);
	REQUIRE(g_capCount == 1);
	const unsigned char* ipb = g_cap + ETH_HLEN; const unsigned char* u = ipb + IP_HLEN_MIN;
	CHECK(ipb[9] == IPPROTO_UDP);
	CHECK(rd32be(ipb + 16) == ipv4(212,77,98,9));
	CHECK(rd16be(u + 2) == 53);
	sc.close(fd);
}

TEST_CASE("TCP connect() returns EINPROGRESS; SYN-ACK completes via sockConnectResult") {
	netSetup();
	Syscalls sc(mountFixture2(), sysSink);
	int fd = sc.sockSocket(AF_INET, SOCK_STREAM, 0);
	unsigned char sa[16]; mkaddr(sa, ipv4(212,77,98,9), 80);
	CHECK(sc.sockConnect(fd, sa, 16) == -EINPROGRESS);
	CHECK(sc.sockConnectResult(fd) == -EINPROGRESS);
	// Capture our SYN to learn the ephemeral port + ISS, then craft the SYN-ACK.
	const unsigned char* ipb = g_cap + ETH_HLEN; const unsigned char* t = ipb + IP_HLEN_MIN;
	uint16_t lport = rd16be(t); uint32_t iss = rd32be(t + 4);
	NetBuf* skb=netbufAlloc(); skb->reserve(0);
	int tot=IP_HLEN_MIN+20; unsigned char* h=skb->put(tot); std::memset(h,0,tot);
	h[0]=0x45; wr16be(h+2,tot); h[8]=64; h[9]=IPPROTO_TCP; wr32be(h+12,ipv4(212,77,98,9)); wr32be(h+16,g_dev.ip);
	wr16be(h+10,inetChecksum(h,IP_HLEN_MIN));
	unsigned char* tt=h+IP_HLEN_MIN; wr16be(tt,80); wr16be(tt+2,lport); wr32be(tt+4,0x900); wr32be(tt+8,iss+1);
	tt[12]=(20/4)<<4; tt[13]=TCP_SYN|TCP_ACK; wr16be(tt+14,4096);
	wr16be(tt+16, inetPseudoChecksum(ipv4(212,77,98,9), g_dev.ip, IPPROTO_TCP, tt, 20));
	skb->dev=&g_dev; ipRx(skb);
	CHECK(sc.sockConnectResult(fd) == 0);              // ESTABLISHED
	sc.close(fd);
}

TEST_CASE("dup() shares the socket (refcount); close one keeps the other") {
	netSetup();
	Syscalls sc(mountFixture2(), sysSink);
	int fd = sc.sockSocket(AF_INET, SOCK_DGRAM, 0);
	unsigned char sa[16]; mkaddr(sa, 0, 6000); sc.sockBind(fd, sa, 16);
	int fd2 = sc.dup(fd);
	CHECK(fd2 >= 3);
	CHECK(sc.isSocketFd(fd2) == true);
	sc.close(fd);                                      // socket stays alive via fd2
	CHECK(sc.isSocketFd(fd2) == true);
	// the surviving fd still receives
	unsigned char p[3] = { 1,2,3 };
	feedUdp(ipv4(1,1,1,1), 7, 6000, p, 3);
	unsigned char buf[8];
	CHECK(sc.read(fd2, buf, sizeof(buf)) == 3);
	sc.close(fd2);
	CHECK(netbufInUse() == 0);
}

TEST_CASE("net ioctls: SIOCGIFADDR / SIOCGIFHWADDR / SIOCGIFMTU") {
	netSetup();
	Syscalls sc(mountFixture2(), sysSink);
	int fd = sc.sockSocket(AF_INET, SOCK_DGRAM, 0);
	unsigned char ifr[32]; std::memset(ifr, 0, sizeof(ifr));
	ifr[0]='e'; ifr[1]='t'; ifr[2]='h'; ifr[3]='0';
	CHECK(sc.netIoctl(fd, 0x8915, ifr) == 0);          // SIOCGIFADDR
	CHECK(rd32be(ifr + 16 + 4) == ipv4(10,0,2,15));    // sockaddr_in addr at union+4
	std::memset(ifr+16, 0, 16);
	CHECK(sc.netIoctl(fd, 0x8927, ifr) == 0);          // SIOCGIFHWADDR
	CHECK(std::memcmp(ifr + 16 + 2, OUR_MAC, 6) == 0); // family(2) then MAC
	std::memset(ifr+16, 0, 16);
	CHECK(sc.netIoctl(fd, 0x8921, ifr) == 0);          // SIOCGIFMTU
	CHECK((ifr[16] | (ifr[17] << 8)) == 1500);
	sc.close(fd);
}

TEST_CASE("EAFNOSUPPORT / ENOTSOCK error paths") {
	netSetup();
	Syscalls sc(mountFixture2(), sysSink);
	// IPv6 socket -> EAFNOSUPPORT (IPv4-only stack).
	CHECK(sc.sockSocket(10 /*AF_INET6*/, SOCK_DGRAM, 0) == -EAFNOSUPPORT);
	// socket op on a non-socket fd (the console) -> ENOTSOCK.
	unsigned char sa[16]; mkaddr(sa, 0, 1);
	CHECK(sc.sockBind(1, sa, 16) == -ENOTSOCK);
	int fd = sc.sockSocket(AF_INET, SOCK_DGRAM, 0);
	// bind with a non-AF_INET family -> EAFNOSUPPORT.
	sa[0] = 99;
	CHECK(sc.sockBind(fd, sa, 16) == -EAFNOSUPPORT);
	sc.close(fd);
}

// FAZA F — AF_PACKET over the fd table: sockaddr_ll bind/recvfrom/sendto + SIOCGIFINDEX.
TEST_CASE("AF_PACKET fd: SIOCGIFINDEX, sockaddr_ll bind/recvfrom/sendto") {
	netSetup();
	Syscalls sc(mountFixture2(), sysSink);
	int fd = sc.sockSocket(AF_PACKET, SOCK_DGRAM, hton16(ETH_P_IP));
	REQUIRE(fd >= 0);

	unsigned char ifr[40]; std::memset(ifr, 0, sizeof ifr); std::memcpy(ifr, "eth0", 4);
	CHECK(sc.netIoctl(fd, 0x8933, ifr) == 0);                       // SIOCGIFINDEX
	int ifx = ifr[16] | (ifr[17] << 8) | (ifr[18] << 16) | (ifr[19] << 24);
	CHECK(ifx == 1);

	unsigned char sll[20]; std::memset(sll, 0, sizeof sll);
	sll[0] = 17; sll[2] = hton16(ETH_P_IP) & 0xff; sll[3] = hton16(ETH_P_IP) >> 8; sll[4] = (unsigned char) ifx;
	CHECK(sc.sockBind(fd, sll, 20) == 0);

	// Feed an IP frame on the wire; recvfrom returns the L3 payload + a filled sockaddr_ll.
	unsigned char ip[20] = { 0x45 };
	NetBuf* skb = netbufAlloc(); skb->reserve(0);
	unsigned char* e = skb->put(ETH_HLEN + 20);
	std::memcpy(e, OUR_MAC, 6); std::memcpy(e + 6, GW_MAC, 6); wr16be(e + 12, ETH_P_IP);
	std::memcpy(e + ETH_HLEN, ip, 20); skb->dev = &g_dev; ethRx(skb);

	char buf[64]; unsigned char from[20]; unsigned fl = 20;
	int n = sc.sockRecvfrom(fd, buf, sizeof buf, 0, from, &fl);
	CHECK(n == 20);
	CHECK(from[0] == 17);                                           // AF_PACKET
	CHECK((from[4] | (from[5] << 8)) == 1);                         // sll_ifindex
	CHECK(std::memcmp(from + 12, GW_MAC, 6) == 0);                  // sll_addr = source MAC

	// sendto (cooked) builds the Ethernet header from the sockaddr_ll.
	unsigned char dst[20]; std::memset(dst, 0, sizeof dst);
	dst[0] = 17; dst[2] = hton16(ETH_P_IP) & 0xff; dst[3] = hton16(ETH_P_IP) >> 8; dst[4] = 1;
	std::memcpy(dst + 12, GW_MAC, 6);
	g_capCount = 0;
	CHECK(sc.sockSendto(fd, "HELLO", 5, 0, dst, 20) == 5);
	REQUIRE(g_capCount == 1);
	CHECK(std::memcmp(g_cap, GW_MAC, 6) == 0);
	CHECK(rd16be(g_cap + 12) == ETH_P_IP);
	CHECK(std::memcmp(g_cap + ETH_HLEN, "HELLO", 5) == 0);
	sc.close(fd);
}

// FAZA F — SIOCADDRT/SIOCDELRT parse a real struct rtentry.
TEST_CASE("SIOCADDRT installs a default route from rtentry; SIOCDELRT removes it") {
	netSetup();
	routeReset();                                                  // start with no routes
	Syscalls sc(mountFixture2(), sysSink);
	int fd = sc.sockSocket(AF_INET, SOCK_DGRAM, 0);

	unsigned char rt[128]; std::memset(rt, 0, sizeof rt);
	rt[4] = 2;                                                     // rt_dst family (addr@8 = 0.0.0.0)
	rt[20] = 2; wr32be(rt + 24, ipv4(10,0,2,2));                   // rt_gateway = 10.0.2.2
	rt[36] = 2;                                                    // rt_genmask (addr@40 = 0.0.0.0)
	rt[52] = 0x03;                                                 // RTF_UP | RTF_GATEWAY

	CHECK(sc.netIoctl(fd, 0x890b, rt) == 0);                       // SIOCADDRT
	NetDevice* od = 0; uint32_t nh = 0;
	REQUIRE(routeLookup(ipv4(8,8,8,8), &od, &nh));
	CHECK(nh == ipv4(10,0,2,2));                                   // default via the gateway

	CHECK(sc.netIoctl(fd, 0x890c, rt) == 0);                       // SIOCDELRT
	CHECK(routeLookup(ipv4(8,8,8,8), &od, &nh) == false);
	sc.close(fd);
}
