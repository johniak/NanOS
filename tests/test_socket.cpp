#include "doctest.h"
#include "Socket.h"
#include "Udp.h"
#include "Raw.h"
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

static unsigned char g_cap[2048]; static int g_capLen, g_capCount;
static int captureTx(NetDevice*, NetBuf* skb) {
	g_capLen = skb->len; std::memcpy(g_cap, skb->head(), skb->len<2048?skb->len:2048);
	g_capCount++; netbufFree(skb); return 0;
}
static void clearCap() { g_capLen=0; g_capCount=0; std::memset(g_cap,0,sizeof(g_cap)); }

static NetDevice g_dev;
static void setup() {
	netReset(); arpReset(); routeReset(); ipReset(); icmpReset(); socketReset();
	std::memset(&g_dev,0,sizeof(g_dev));
	g_dev.name[0]='e';g_dev.name[1]='t';g_dev.name[2]='h';g_dev.name[3]='0';
	std::memcpy(g_dev.mac,OUR_MAC,6);
	g_dev.mtu=1500; g_dev.flags=NETIF_UP|NETIF_RUNNING|NETIF_BROADCAST;
	g_dev.ip=ipv4(10,0,2,15); g_dev.netmask=ipv4(255,255,255,0); g_dev.broadcast=ipv4(10,0,2,255);
	g_dev.tx=captureTx;
	ethInit(); arpInit(); ipInit(); icmpInit(); udpInit(); rawInit();
	routeAddDefault(&g_dev, ipv4(10,0,2,2));
	clearCap();
}
static void seedArp(uint32_t ip, const uint8_t mac[6]) {
	NetBuf* s=netbufAlloc(); s->reserve(0);
	unsigned char* e=s->put(ETH_HLEN+ARP_PLEN);
	std::memcpy(e,OUR_MAC,6); std::memcpy(e+6,mac,6); wr16be(e+12,ETH_P_ARP);
	unsigned char* a=e+ETH_HLEN; wr16be(a,ARP_HTYPE_ETH); wr16be(a+2,ETH_P_IP); a[4]=6;a[5]=4;
	wr16be(a+6,ARP_OP_REPLY); std::memcpy(a+8,mac,6); wr32be(a+14,ip); std::memcpy(a+18,OUR_MAC,6); wr32be(a+24,g_dev.ip);
	s->dev=&g_dev; ethRx(s);
}
// Feed an IP+UDP packet (from src:sport to our:dport) to the stack.
static void feedUdp(uint32_t src, uint16_t sport, uint16_t dport, const unsigned char* data, int dlen) {
	NetBuf* skb=netbufAlloc(); skb->reserve(0);
	int ulen=8+dlen, tot=IP_HLEN_MIN+ulen;
	unsigned char* h=skb->put(tot); std::memset(h,0,tot);
	h[0]=0x45; wr16be(h+2,tot); h[8]=64; h[9]=IPPROTO_UDP; wr32be(h+12,src); wr32be(h+16,g_dev.ip);
	wr16be(h+10, inetChecksum(h,IP_HLEN_MIN));
	unsigned char* u=h+IP_HLEN_MIN; wr16be(u,sport); wr16be(u+2,dport); wr16be(u+4,ulen);
	if(dlen) std::memcpy(u+8,data,dlen);
	// pseudo-header checksum
	unsigned char ph[12]; wr32be(ph,src); wr32be(ph+4,g_dev.ip); ph[8]=0; ph[9]=IPPROTO_UDP; wr16be(ph+10,ulen);
	uint32_t sum=inetChecksumAccum(ph,12,0); sum=inetChecksumAccum(u,ulen,sum);
	uint16_t c=inetChecksumFinish(sum); wr16be(u+6, c?c:0xFFFF);
	skb->dev=&g_dev; ipRx(skb);
}

// ---------------- socket creation / options ----------------

TEST_CASE("socketCreate: AF_INET DGRAM ok; IPv6 -> EAFNOSUPPORT; STREAM -> not yet") {
	setup();
	int err=-1;
	Socket* u=socketCreate(AF_INET, SOCK_DGRAM, 0, &err);
	REQUIRE(u != nullptr); CHECK(err==0);
	CHECK(socketCreate(10 /*AF_INET6*/, SOCK_DGRAM, 0, &err) == nullptr);
	CHECK(err == -SOCK_EAFNOSUPPORT);
	CHECK(socketCreate(AF_INET, SOCK_STREAM, 0, &err) == nullptr);
	CHECK(err == -SOCK_EPROTONOSUPPORT);   // TCP arrives in FAZA 8
	socketClose(u);
}

TEST_CASE("bind: ephemeral on port 0, EADDRINUSE on a taken port") {
	setup();
	Socket* a=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr);
	CHECK(socketBind(a, 0, 0) == 0);
	uint32_t ip; uint16_t port; socketGetSockName(a,&ip,&port);
	CHECK(port >= 32768);                  // ephemeral range
	Socket* b=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr);
	CHECK(socketBind(b, 0, 5353) == 0);
	Socket* c=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr);
	CHECK(socketBind(c, 0, 5353) == -SOCK_EADDRINUSE);
	socketClose(a); socketClose(b); socketClose(c);
}

TEST_CASE("setsockopt/getsockopt: SO_TYPE, SO_BROADCAST, SO_RCVBUF, SO_ERROR") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr);
	int v=1; CHECK(socketSetOpt(s,SOL_SOCKET,SO_BROADCAST,&v,sizeof(v))==0);
	CHECK(s->broadcast == true);
	int rb=8192; CHECK(socketSetOpt(s,SOL_SOCKET,SO_RCVBUF,&rb,sizeof(rb))==0);
	int out; unsigned len=sizeof(out);
	CHECK(socketGetOpt(s,SOL_SOCKET,SO_TYPE,&out,&len)==0); CHECK(out==SOCK_DGRAM);
	len=sizeof(out); CHECK(socketGetOpt(s,SOL_SOCKET,SO_RCVBUF,&out,&len)==0); CHECK(out==8192);
	s->soError=SOCK_ECONNREFUSED; len=sizeof(out);
	CHECK(socketGetOpt(s,SOL_SOCKET,SO_ERROR,&out,&len)==0); CHECK(out==SOCK_ECONNREFUSED);
	CHECK(s->soError==0);   // SO_ERROR read-and-clear
	socketClose(s);
}

// ---------------- UDP send / receive ----------------

TEST_CASE("udpSend emits a valid UDP datagram (header + pseudo-header checksum)") {
	setup(); seedArp(ipv4(10,0,2,2), GW_MAC); clearCap();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr);
	socketConnect(s, ipv4(212,77,98,9), 53);
	const char* q="\x12\x34\x01\x00query";
	CHECK(socketSendTo(s, q, 12, 0, 0) == 12);
	REQUIRE(g_capCount==1);
	const unsigned char* h=g_cap+ETH_HLEN; const unsigned char* u=h+IP_HLEN_MIN;
	CHECK(h[9]==IPPROTO_UDP);
	CHECK(rd16be(u+2)==53);                       // dst port
	CHECK(rd16be(u+4)==(uint16_t)(8+12));         // udp length
	// Recompute the pseudo-header checksum over the captured datagram -> must validate to 0.
	unsigned char ph[12]; wr32be(ph,ipv4(10,0,2,15)); wr32be(ph+4,ipv4(212,77,98,9));
	ph[8]=0; ph[9]=IPPROTO_UDP; wr16be(ph+10, 8+12);
	uint32_t sum=inetChecksumAccum(ph,12,0); sum=inetChecksumAccum(u, 8+12, sum);
	CHECK(inetChecksumFinish(sum)==0);
	socketClose(s);
}

TEST_CASE("UDP receive: bound socket gets the datagram + source address") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr);
	socketBind(s, 0, 9999);
	unsigned char payload[10]; for(int i=0;i<10;i++) payload[i]=(unsigned char)(0x50+i);
	feedUdp(ipv4(8,8,8,8), 4444, 9999, payload, 10);
	CHECK(socketReadable(s) == true);
	unsigned char buf[64]; uint32_t sip; uint16_t sport;
	int n=socketRecvFrom(s, buf, sizeof(buf), &sip, &sport, 0);
	CHECK(n==10);
	CHECK(std::memcmp(buf, payload, 10)==0);
	CHECK(sip==ipv4(8,8,8,8));
	CHECK(sport==4444);
	CHECK(socketRecvFrom(s, buf, sizeof(buf), &sip, &sport, 0) == -SOCK_EAGAIN);   // drained
	socketClose(s);
}

TEST_CASE("MSG_PEEK leaves the datagram queued") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketBind(s,0,1234);
	unsigned char p[4]={1,2,3,4}; feedUdp(ipv4(1,1,1,1), 7, 1234, p, 4);
	unsigned char buf[16];
	CHECK(socketRecvFrom(s,buf,sizeof(buf),nullptr,nullptr, MSG_PEEK)==4);
	CHECK(socketReadable(s)==true);                          // still there
	CHECK(socketRecvFrom(s,buf,sizeof(buf),nullptr,nullptr, 0)==4);
	CHECK(socketReadable(s)==false);                         // now consumed
	socketClose(s);
}

TEST_CASE("connected socket demux: only the matching peer's datagram is delivered") {
	setup();
	Socket* a=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketBind(a,0,5000); socketConnect(a, ipv4(8,8,8,8), 53);
	Socket* b=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketBind(b,0,5000);   // wildcard listener, same port? no
	// (b on a different port to avoid EADDRINUSE)
	socketClose(b); b=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketBind(b,0,5001);
	unsigned char p[2]={9,9};
	feedUdp(ipv4(8,8,8,8), 53, 5000, p, 2);                 // from a's peer -> a
	CHECK(socketReadable(a)==true);
	feedUdp(ipv4(9,9,9,9), 53, 5000, p, 2);                 // wrong peer, no wildcard on 5000 -> dropped
	unsigned char buf[8]; int n=0, got=0;
	while ((n=socketRecvFrom(a,buf,sizeof(buf),nullptr,nullptr,0))>0) got++;
	CHECK(got==1);                                          // only the matching-peer datagram
	socketClose(a); socketClose(b);
}

TEST_CASE("UDP to an unbound port -> ICMP port-unreachable") {
	setup(); seedArp(ipv4(10,0,2,2), GW_MAC); clearCap();   // error routes back via the gateway
	unsigned char p[4]={1,2,3,4};
	feedUdp(ipv4(8,8,8,8), 1111, 2222, p, 4);              // nobody bound to 2222
	REQUIRE(g_capCount==1);
	const unsigned char* h=g_cap+ETH_HLEN; const unsigned char* m=h+IP_HLEN_MIN;
	CHECK(h[9]==IPPROTO_ICMP);
	CHECK(m[0]==ICMP_DEST_UNREACH);
	CHECK(m[1]==ICMP_PORT_UNREACH);
}

// ---------------- refcount ----------------

TEST_CASE("refcount: socketRef + close, freed at zero (fork/dup semantics)") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketBind(s,0,7777);
	socketRef(s);                       // a dup/fork shares it
	CHECK(s->refs==2);
	socketClose(s);                     // one closer
	CHECK(s->used==true);               // still alive
	CHECK(s->refs==1);
	socketClose(s);                     // last closer
	CHECK(s->used==false);              // freed
}

TEST_CASE("socket edge cases: peer name, poll, bad opts, bind-twice, unconnected send") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr);
	uint32_t ip; uint16_t port;
	CHECK(socketGetPeerName(s,&ip,&port) == -SOCK_ENOTCONN);
	socketConnect(s, ipv4(1,2,3,4), 80);
	CHECK(socketGetPeerName(s,&ip,&port)==0); CHECK(ip==ipv4(1,2,3,4)); CHECK(port==80);
	// poll: writable always, not readable yet.
	int p=socketPoll(s); CHECK((p & 0x004)!=0); CHECK((p & 0x001)==0);
	CHECK(socketWritable(s)==true);
	// invalid options.
	int v=1;
	CHECK(socketSetOpt(s,SOL_SOCKET,9999,&v,sizeof(v)) == -SOCK_EINVAL);
	CHECK(socketSetOpt(s,SOL_SOCKET,SO_BROADCAST,&v,2) == -SOCK_EINVAL);   // len < int
	unsigned len=2; CHECK(socketGetOpt(s,SOL_SOCKET,SO_TYPE,&v,&len) == -SOCK_EINVAL);
	len=sizeof(v); CHECK(socketGetOpt(s,SOL_SOCKET,9999,&v,&len) == -SOCK_EINVAL);
	// bind after connect already bound -> EINVAL.
	CHECK(socketBind(s, 0, 1000) == -SOCK_EINVAL);
	socketClose(s);
	// unconnected sendto with no destination.
	Socket* u=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr);
	CHECK(socketSendTo(u, "x", 1, 0, 0) == -SOCK_ENOTCONN);
	socketClose(u);
}

TEST_CASE("socket receive ring fills then drops (deliver returns false)") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketBind(s,0,6000);
	int delivered=0;
	for (int i=0;i<Socket::RXQ+4;i++) {
		NetBuf* b=netbufAlloc(); b->reserve(0); b->put(4);
		if (socketDeliver(s, b, ipv4(1,1,1,1), 1)) delivered++; else netbufFree(b);
	}
	CHECK(delivered == Socket::RXQ);   // ring caps at RXQ; excess dropped
	socketReset();
}

TEST_CASE("wildcard listener (INADDR_ANY, unconnected) receives any peer") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketBind(s,0,7000);
	unsigned char p[2]={5,5};
	feedUdp(ipv4(3,3,3,3), 1, 7000, p, 2);
	feedUdp(ipv4(4,4,4,4), 2, 7000, p, 2);
	unsigned char buf[8]; int got=0;
	while (socketRecvFrom(s,buf,sizeof(buf),nullptr,nullptr,0) > 0) got++;
	CHECK(got==2);
	socketClose(s);
}

TEST_CASE("udpRx hardening: runt, bad checksum, broadcast-no-error") {
	setup(); seedArp(ipv4(10,0,2,2), GW_MAC);
	// Runt UDP (length field < 8).
	NetBuf* r=netbufAlloc(); r->reserve(0);
	unsigned char* h=r->put(IP_HLEN_MIN+4); std::memset(h,0,IP_HLEN_MIN+4);
	h[0]=0x45; wr16be(h+2,IP_HLEN_MIN+4); h[8]=64; h[9]=IPPROTO_UDP; wr32be(h+12,ipv4(8,8,8,8)); wr32be(h+16,g_dev.ip);
	wr16be(h+10,inetChecksum(h,IP_HLEN_MIN));
	r->dev=&g_dev; ipRx(r);            // 4-byte "UDP" -> dropped, no crash
	CHECK(netbufInUse()==0);
	// Broadcast to an unbound port -> NO ICMP error (don't answer broadcasts).
	clearCap();
	NetBuf* b=netbufAlloc(); b->reserve(0);
	int ulen=8, tot=IP_HLEN_MIN+ulen;
	unsigned char* H=b->put(tot); std::memset(H,0,tot);
	H[0]=0x45; wr16be(H+2,tot); H[8]=64; H[9]=IPPROTO_UDP; wr32be(H+12,ipv4(8,8,8,8)); wr32be(H+16,0xFFFFFFFFu);
	wr16be(H+10,inetChecksum(H,IP_HLEN_MIN));
	unsigned char* u=H+IP_HLEN_MIN; wr16be(u,1); wr16be(u+2,9); wr16be(u+4,ulen); wr16be(u+6,0);  // csum 0 = none
	b->dev=&g_dev; ipRx(b);
	CHECK(g_capCount==0);              // broadcast to closed port: silently dropped

	// UDP with a wrong (non-zero) checksum -> dropped, never delivered.
	Socket* sk=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketBind(sk,0,8800);
	NetBuf* c=netbufAlloc(); c->reserve(0);
	int ul=8+2, t2=IP_HLEN_MIN+ul;
	unsigned char* CH=c->put(t2); std::memset(CH,0,t2);
	CH[0]=0x45; wr16be(CH+2,t2); CH[8]=64; CH[9]=IPPROTO_UDP; wr32be(CH+12,ipv4(8,8,8,8)); wr32be(CH+16,g_dev.ip);
	wr16be(CH+10,inetChecksum(CH,IP_HLEN_MIN));
	unsigned char* cu=CH+IP_HLEN_MIN; wr16be(cu,1); wr16be(cu+2,8800); wr16be(cu+4,ul); wr16be(cu+6,0x1234); // bad csum
	c->dev=&g_dev; ipRx(c);
	CHECK(socketReadable(sk)==false);  // dropped on checksum mismatch
	socketClose(sk);
}

TEST_CASE("udpSend rejects oversize payloads (EMSGSIZE)") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_DGRAM,0,nullptr); socketConnect(s, ipv4(8,8,8,8), 53);
	static unsigned char big[70000];
	CHECK(socketSendTo(s, big, 70000, 0, 0) == -SOCK_EMSGSIZE);
	socketClose(s);
}

// ---------------- RAW (ping) ----------------

TEST_CASE("rawSend emits the caller's ICMP message inside an IP packet") {
	setup(); seedArp(ipv4(10,0,2,2), GW_MAC); clearCap();
	Socket* s=socketCreate(AF_INET,SOCK_RAW,IPPROTO_ICMP,nullptr);
	// An 8-byte ICMP echo request, checksum filled by the "app".
	unsigned char icmp[8]; std::memset(icmp,0,8); icmp[0]=ICMP_ECHO_REQUEST; wr16be(icmp+4,0x1234); wr16be(icmp+6,1);
	wr16be(icmp+2, inetChecksum(icmp,8));
	CHECK(socketSendTo(s, icmp, 8, ipv4(212,77,98,9), 0) == 8);
	REQUIRE(g_capCount==1);
	const unsigned char* h=g_cap+ETH_HLEN;
	CHECK(h[9]==IPPROTO_ICMP);
	CHECK(rd32be(h+16)==ipv4(212,77,98,9));
	const unsigned char* m=h+IP_HLEN_MIN;
	CHECK(m[0]==ICMP_ECHO_REQUEST);
	socketClose(s);
}

TEST_CASE("RAW ICMP receive returns the full IP datagram (Linux raw-socket semantics)") {
	setup();
	Socket* s=socketCreate(AF_INET,SOCK_RAW,IPPROTO_ICMP,nullptr);
	// Feed an ICMP echo reply (from the gateway) through the IP path.
	NetBuf* skb=netbufAlloc(); skb->reserve(0);
	int icmpLen=8+4, tot=IP_HLEN_MIN+icmpLen;
	unsigned char* h=skb->put(tot); std::memset(h,0,tot);
	h[0]=0x45; wr16be(h+2,tot); h[8]=64; h[9]=IPPROTO_ICMP; wr32be(h+12,ipv4(212,77,98,9)); wr32be(h+16,g_dev.ip);
	wr16be(h+10, inetChecksum(h,IP_HLEN_MIN));
	unsigned char* m=h+IP_HLEN_MIN; m[0]=ICMP_ECHO_REPLY; wr16be(m+4,0x1234); wr16be(m+6,1);
	for(int i=0;i<4;i++) m[8+i]=(unsigned char)(0xE0+i);
	wr16be(m+2, inetChecksum(m, icmpLen));
	skb->dev=&g_dev; ipRx(skb);
	// recvfrom must return the whole IP packet: IP header (0x45...) + ICMP.
	CHECK(socketReadable(s)==true);
	unsigned char buf[128]; uint32_t sip;
	int n=socketRecvFrom(s, buf, sizeof(buf), &sip, nullptr, 0);
	CHECK(n == tot);                       // 20 IP + 12 ICMP
	CHECK(buf[0]==0x45);                    // starts with the IP header
	CHECK(buf[9]==IPPROTO_ICMP);
	CHECK(buf[IP_HLEN_MIN]==ICMP_ECHO_REPLY);
	CHECK(sip==ipv4(212,77,98,9));
	socketClose(s);
}
