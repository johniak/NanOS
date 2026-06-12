// FAZA D — TCP options & timers on the wire (window scaling, timestamps, SACK, delayed ACK,
// persist, keepalive). Self-contained: builds segments with arbitrary TCP options and inspects
// what our stack emits, so the negotiation is checked field-by-field like Linux.
#include "doctest.h"
#include "Tcp.h"
#include "Ip.h"
#include "Route.h"
#include "Ether.h"
#include "Arp.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include "Socket.h"
#include "Udp.h"
#include "Raw.h"
#include "Icmp.h"
#include <cstdint>
#include <cstring>

using namespace kernel;

static const uint8_t OUR_MAC[6] = { 0xaa,0xbb,0xcc,0x00,0x00,0x01 };
static NetDevice g_dev;
static unsigned char g_cap[2048]; static int g_capLen, g_capCount;
static int captureTx(NetDevice*, NetBuf* skb) {
	g_capLen=skb->len; std::memcpy(g_cap,skb->head(),skb->len<2048?skb->len:2048); g_capCount++;
	netbufFree(skb); return 0;
}
static void clearCap(){ g_capLen=0; g_capCount=0; std::memset(g_cap,0,sizeof(g_cap)); }
static unsigned g_now;
static unsigned tclock(){ return g_now; }

static void setup() {
	netReset(); arpReset(); routeReset(); ipReset(); icmpReset(); socketReset(); tcpReset();
	std::memset(&g_dev,0,sizeof(g_dev));
	g_dev.name[0]='e';g_dev.name[1]='t';g_dev.name[2]='h';g_dev.name[3]='0';
	std::memcpy(g_dev.mac,OUR_MAC,6);
	g_dev.mtu=1500; g_dev.flags=NETIF_UP|NETIF_RUNNING|NETIF_BROADCAST;
	g_dev.ip=ipv4(10,0,2,15); g_dev.netmask=ipv4(255,255,255,0); g_dev.broadcast=ipv4(10,0,2,255);
	g_dev.tx=captureTx;
	ethInit(); arpInit(); ipInit(); icmpInit(); udpInit(); rawInit(); tcpInit();
	tcpSetNewSockHook(socketCreateRaw);
	tcpSetClock(tclock); g_now=10000;
	routeAddDefault(&g_dev, ipv4(10,0,2,2));
	// seed ARP for the gateway so TCP segments are emitted immediately (not queued behind ARP).
	static const uint8_t GW_MAC[6]={0x52,0x55,0x0a,0x00,0x02,0x02};
	NetBuf* sa=netbufAlloc(); sa->reserve(0);
	unsigned char* e=sa->put(ETH_HLEN+ARP_PLEN);
	std::memcpy(e,OUR_MAC,6); std::memcpy(e+6,GW_MAC,6); wr16be(e+12,ETH_P_ARP);
	unsigned char* a=e+ETH_HLEN; wr16be(a,ARP_HTYPE_ETH); wr16be(a+2,ETH_P_IP); a[4]=6;a[5]=4;
	wr16be(a+6,ARP_OP_REPLY); std::memcpy(a+8,GW_MAC,6); wr32be(a+14,ipv4(10,0,2,2));
	std::memcpy(a+18,OUR_MAC,6); wr32be(a+24,g_dev.ip);
	sa->dev=&g_dev; ethRx(sa);
	clearCap();
}

struct OptSeg { uint16_t sport,dport; uint32_t seq,ack; uint8_t flags; int doff,plen; const unsigned char* opt; const unsigned char* data; };
static bool parseCap(OptSeg* o) {
	if (g_capLen < ETH_HLEN+IP_HLEN_MIN+20) return false;
	const unsigned char* ip=g_cap+ETH_HLEN; int ihl=(ip[0]&0xf)*4; int iptot=rd16be(ip+2);
	const unsigned char* t=ip+ihl;
	o->sport=rd16be(t); o->dport=rd16be(t+2); o->seq=rd32be(t+4); o->ack=rd32be(t+8);
	o->doff=(t[12]>>4)*4; o->flags=t[13]; o->opt=t+20; o->data=t+o->doff; o->plen=iptot-ihl-o->doff;
	if (inetPseudoChecksum(rd32be(ip+12), rd32be(ip+16), IPPROTO_TCP, t, iptot-ihl) != 0) return false;
	return true;
}
// Find a TCP option of `kind` in the captured segment; returns its length byte (or 0 if absent).
static int capOption(const OptSeg& s, uint8_t kind, const unsigned char** val) {
	int optBytes = s.doff - 20;
	for (int i=0; i < optBytes; ) {
		uint8_t k=s.opt[i];
		if (k==0) break;
		if (k==1) { i++; continue; }
		if (i+1 >= optBytes) break;
		uint8_t olen=s.opt[i+1]; if (olen<2 || i+olen > optBytes) break;
		if (k==kind) { if (val) *val=s.opt+i+2; return olen; }
		i+=olen;
	}
	return 0;
}
// Feed a TCP segment with an arbitrary options blob.
static void feedOpt(uint32_t peer, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                    uint8_t flags, uint16_t win, const unsigned char* opt, int optLen,
                    const unsigned char* data, int dlen) {
	NetBuf* skb=netbufAlloc(); skb->reserve(0);
	int tlen=20+optLen+dlen, tot=IP_HLEN_MIN+tlen;
	unsigned char* h=skb->put(tot); std::memset(h,0,tot);
	h[0]=0x45; wr16be(h+2,tot); h[8]=64; h[9]=IPPROTO_TCP; wr32be(h+12,peer); wr32be(h+16,g_dev.ip);
	wr16be(h+10,inetChecksum(h,IP_HLEN_MIN));
	unsigned char* t=h+IP_HLEN_MIN;
	wr16be(t,sport); wr16be(t+2,dport); wr32be(t+4,seq); wr32be(t+8,ack);
	t[12]=(unsigned char)(((20+optLen)/4)<<4); t[13]=flags; wr16be(t+14,win); wr16be(t+16,0);
	if (optLen) std::memcpy(t+20,opt,optLen);
	if (dlen) std::memcpy(t+20+optLen,data,dlen);
	wr16be(t+16, inetPseudoChecksum(peer, g_dev.ip, IPPROTO_TCP, t, tlen));
	skb->dev=&g_dev; ipRx(skb);
}

// ---------------------------------------------------------------------------

TEST_CASE("window scale: our SYN advertises wscale; peer's shift scales its window after handshake") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Socket* s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	REQUIRE(socketConnect(s, peer, 80) == 0);
	OptSeg syn; REQUIRE(parseCap(&syn));
	CHECK((syn.flags & TCP_SYN) != 0);
	const unsigned char* ws=nullptr;
	REQUIRE(capOption(syn, 3, &ws) == 3);          // window-scale option present in our SYN
	uint16_t lport=syn.sport; uint32_t iss=syn.seq;
	clearCap();

	// SYN-ACK that negotiates wscale 7 and advertises window field 1000 (unscaled in the SYN-ACK).
	unsigned char opt[]={2,4,0x05,0xB4, 1, 3,3,7};   // MSS 1460, NOP, wscale 7
	feedOpt(peer, 80, lport, 0x50000, iss+1, TCP_SYN|TCP_ACK, 1000, opt, sizeof opt, nullptr, 0);
	CHECK(tcpState(s) == TCP_ESTABLISHED);
	CHECK(tcpSndWnd(s) == 1000);                    // SYN-ACK window is NOT scaled (RFC 7323 §2.2)
	clearCap();

	// Send data, then have the peer ACK it advertising window field 4 — now scaled: 4 << 7 = 512.
	const char* d="hello"; tcpSend(s, d, 5);
	clearCap();
	feedOpt(peer, 80, lport, 0x50001, iss+1+5, TCP_ACK, 4, nullptr, 0, nullptr, 0);
	CHECK(tcpSndWnd(s) == (4u << 7));               // peer's shift honoured
	socketClose(s);
}

TEST_CASE("window scale: peer SYN-ACK without wscale -> scaling disabled both ways") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Socket* s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	REQUIRE(socketConnect(s, peer, 80) == 0);
	OptSeg syn; REQUIRE(parseCap(&syn));
	uint16_t lport=syn.sport; uint32_t iss=syn.seq;
	clearCap();

	// SYN-ACK with MSS only (no wscale) -> no scaling.
	unsigned char opt[]={2,4,0x05,0xB4};
	feedOpt(peer, 80, lport, 0x50000, iss+1, TCP_SYN|TCP_ACK, 100, opt, sizeof opt, nullptr, 0);
	CHECK(tcpState(s) == TCP_ESTABLISHED);
	const char* d="hi"; tcpSend(s, d, 2);
	clearCap();
	feedOpt(peer, 80, lport, 0x50001, iss+1+2, TCP_ACK, 4, nullptr, 0, nullptr, 0);
	CHECK(tcpSndWnd(s) == 4);                       // unscaled: 4, not 4<<anything
	socketClose(s);
}

TEST_CASE("timestamps: SYN offers TS; after negotiation our segments echo the peer's TSval") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Socket* s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	REQUIRE(socketConnect(s, peer, 80) == 0);
	OptSeg syn; REQUIRE(parseCap(&syn));
	const unsigned char* ts=nullptr;
	REQUIRE(capOption(syn, 8, &ts) == 10);          // TS option present in our SYN
	uint32_t ourSynTsVal = rd32be(ts);
	uint16_t lport=syn.sport; uint32_t iss=syn.seq;
	clearCap();

	// SYN-ACK carrying TS: peer TSval = 0x1111, TSecr echoes our SYN's TSval.
	unsigned char opt[]={2,4,0x05,0xB4, 1,1, 8,10, 0,0,0x11,0x11, 0,0,0,0, 1, 3,3,7};
	wr32be(opt+12, ourSynTsVal);                    // TSecr field
	feedOpt(peer, 80, lport, 0x50000, iss+1, TCP_SYN|TCP_ACK, 1000, opt, sizeof opt, nullptr, 0);
	REQUIRE(tcpState(s) == TCP_ESTABLISHED);

	// Our ACK of the SYN-ACK must carry TS, echoing the peer's TSval (0x1111) in TSecr.
	OptSeg ackseg; REQUIRE(parseCap(&ackseg));
	const unsigned char* ts2=nullptr;
	REQUIRE(capOption(ackseg, 8, &ts2) == 10);
	CHECK(rd32be(ts2+4) == 0x1111u);                // TSecr echoes peer's latest TSval
	socketClose(s);
}

TEST_CASE("timestamps: PAWS drops an in-window segment whose timestamp is stale") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Socket* s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	REQUIRE(socketConnect(s, peer, 80) == 0);
	OptSeg syn; REQUIRE(parseCap(&syn));
	uint16_t lport=syn.sport; uint32_t iss=syn.seq; uint32_t pseq=0x50000;
	clearCap();
	// SYN-ACK with TS, peer TSval = 0x1000 -> becomes ts_recent.
	unsigned char sa[]={2,4,0x05,0xB4, 1,1, 8,10, 0,0,0x10,0x00, 0,0,0,0, 1, 3,3,7};
	feedOpt(peer, 80, lport, pseq, iss+1, TCP_SYN|TCP_ACK, 1000, sa, sizeof sa, nullptr, 0);
	REQUIRE(tcpState(s) == TCP_ESTABLISHED);
	pseq += 1; clearCap();

	// A data segment with a STALE timestamp (0x0500 < ts_recent 0x1000) must be dropped by PAWS.
	unsigned char tsOld[]={1,1, 8,10, 0,0,0x05,0x00, 0,0,0,0};
	feedOpt(peer, 80, lport, pseq, iss+1, TCP_ACK, 1000, tsOld, sizeof tsOld, (const unsigned char*)"abc", 3);
	char buf[16]; uint32_t si; uint16_t sp;
	CHECK(socketRecvFrom(s, buf, sizeof buf, &si, &sp, 0) == -SOCK_EAGAIN);   // PAWS dropped it

	// The same data with a FRESH timestamp (0x2000 > ts_recent) is accepted.
	unsigned char tsNew[]={1,1, 8,10, 0,0,0x20,0x00, 0,0,0,0};
	feedOpt(peer, 80, lport, pseq, iss+1, TCP_ACK, 1000, tsNew, sizeof tsNew, (const unsigned char*)"abc", 3);
	int n = socketRecvFrom(s, buf, sizeof buf, &si, &sp, 0);
	CHECK(n == 3);
	CHECK(std::memcmp(buf, "abc", 3) == 0);
	socketClose(s);
}

TEST_CASE("SACK: SYN offers SACK-permitted; an out-of-order segment makes our ACK carry a SACK block") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Socket* s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	REQUIRE(socketConnect(s, peer, 80) == 0);
	OptSeg syn; REQUIRE(parseCap(&syn));
	REQUIRE(capOption(syn, 4, nullptr) == 2);       // SACK-permitted present in our SYN
	uint16_t lport=syn.sport; uint32_t iss=syn.seq; uint32_t pseq=0x50000;
	clearCap();

	unsigned char sa[]={2,4,0x05,0xB4, 4,2, 1,1};   // SYN-ACK: MSS + SACK-permitted + 2 NOPs (4-aligned)
	feedOpt(peer, 80, lport, pseq, iss+1, TCP_SYN|TCP_ACK, 4096, sa, sizeof sa, nullptr, 0);
	REQUIRE(tcpState(s) == TCP_ESTABLISHED);
	uint32_t base = pseq + 1;                       // = our rcv_nxt
	clearCap();

	feedOpt(peer, 80, lport, base,   iss+1, TCP_ACK, 4096, nullptr,0, (const unsigned char*)"AAA",3);   // in-order
	clearCap();
	feedOpt(peer, 80, lport, base+6, iss+1, TCP_ACK, 4096, nullptr,0, (const unsigned char*)"CCC",3);   // hole at base+3
	OptSeg ack; REQUIRE(parseCap(&ack));
	const unsigned char* sack=nullptr;
	REQUIRE(capOption(ack, 5, &sack) == 10);        // exactly one SACK block (2 + 8)
	CHECK(rd32be(sack)   == base+6);                // left edge of the out-of-order range
	CHECK(rd32be(sack+4) == base+9);                // right edge
	socketClose(s);
}

TEST_CASE("SACK: a fast retransmit is bounded at the SACKed edge (only the hole is resent)") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Socket* s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	REQUIRE(socketConnect(s, peer, 80) == 0);
	OptSeg syn; REQUIRE(parseCap(&syn));
	uint16_t lport=syn.sport; uint32_t iss=syn.seq; uint32_t pseq=0x50000;
	clearCap();
	unsigned char sa[]={2,4,0x05,0xB4, 4,2, 1,1};   // SYN-ACK: MSS 1460 + SACK-permitted (4-aligned)
	feedOpt(peer, 80, lport, pseq, iss+1, TCP_SYN|TCP_ACK, 4096, sa, sizeof sa, nullptr, 0);
	REQUIRE(tcpState(s) == TCP_ESTABLISHED);
	uint32_t U = iss+1;                             // our first data seq
	clearCap();

	tcpSend(s, "ABCDEFGHI", 9);                     // [U, U+9) on the wire
	clearCap();

	// Peer keeps the cumulative ACK at U (the head is the hole) but SACKs [U+3, U+9) as received.
	auto dupack = [&](){
		unsigned char so[]={1,1, 5,10, 0,0,0,0, 0,0,0,0};
		wr32be(so+4, U+3); wr32be(so+8, U+9);
		feedOpt(peer, 80, lport, pseq, U, TCP_ACK, 4096, so, sizeof so, nullptr, 0);
	};
	dupack(); dupack(); dupack();                   // three duplicate ACKs -> fast retransmit
	OptSeg rx; REQUIRE(parseCap(&rx));
	CHECK(rx.seq == U);                             // retransmit starts at the hole ...
	CHECK(rx.plen == 3);                            // ... and stops at the SACKed edge (3 B, not 9)
	socketClose(s);
}

// Establish a plain connection (MSS only, no TS/SACK) and return (lport, our snd seq, rcv base).
static void establishPlain(Socket* s, uint32_t peer, uint16_t* lport, uint32_t* rcvBase) {
	REQUIRE(socketConnect(s, peer, 80) == 0);
	OptSeg syn; REQUIRE(parseCap(&syn));
	*lport = syn.sport; uint32_t iss = syn.seq; uint32_t pseq = 0x60000;
	clearCap();
	unsigned char sa[]={2,4,0x05,0xB4};             // MSS only (4 bytes, 4-aligned)
	feedOpt(peer, 80, *lport, pseq, iss+1, TCP_SYN|TCP_ACK, 4096, sa, sizeof sa, nullptr, 0);
	REQUIRE(tcpState(s) == TCP_ESTABLISHED);
	*rcvBase = pseq + 1;
	clearCap();
}

TEST_CASE("delayed ACK: a lone in-order segment is acked by the timer, not immediately") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Socket* s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	uint16_t lport; uint32_t base; establishPlain(s, peer, &lport, &base);

	feedOpt(peer, 80, lport, base, 0, TCP_ACK, 4096, nullptr, 0, (const unsigned char*)"AAA", 3);
	CHECK(g_capCount == 0);                         // no immediate ACK for a single segment
	g_now += 50; tcpTick(g_now);                    // timer fires -> delayed ACK
	OptSeg ack; REQUIRE(parseCap(&ack));
	CHECK((ack.flags & TCP_ACK) != 0);
	CHECK(ack.ack == base+3);
	socketClose(s);
}

TEST_CASE("delayed ACK: the second in-order segment is acked immediately") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Socket* s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	uint16_t lport; uint32_t base; establishPlain(s, peer, &lport, &base);

	feedOpt(peer, 80, lport, base,   0, TCP_ACK, 4096, nullptr, 0, (const unsigned char*)"AAA", 3);
	CHECK(g_capCount == 0);                         // first segment: delayed
	feedOpt(peer, 80, lport, base+3, 0, TCP_ACK, 4096, nullptr, 0, (const unsigned char*)"BBB", 3);
	OptSeg ack; REQUIRE(parseCap(&ack));            // second segment: immediate ACK
	CHECK(ack.ack == base+6);
	socketClose(s);
}
