#include "doctest.h"
#include "Tcp.h"
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
#include <initializer_list>

using namespace kernel;

static const uint8_t OUR_MAC[6] = { 0xaa,0xbb,0xcc,0x00,0x00,0x01 };
static const uint8_t GW_MAC[6]  = { 0x52,0x55,0x0a,0x00,0x02,0x02 };
static const uint32_t PEER = 0;   // set per test

static unsigned char g_cap[2048]; static int g_capLen, g_capCount;
static int captureTx(NetDevice*, NetBuf* skb) {
	g_capLen=skb->len; std::memcpy(g_cap,skb->head(),skb->len<2048?skb->len:2048); g_capCount++;
	netbufFree(skb); return 0;
}
static void clearCap(){ g_capLen=0; g_capCount=0; std::memset(g_cap,0,sizeof(g_cap)); }

static NetDevice g_dev;
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
	// seed ARP so segments go straight out (gateway + on-link peer)
	for (uint32_t ip : { ipv4(10,0,2,2), ipv4(10,0,2,80) }) {
		NetBuf* s=netbufAlloc(); s->reserve(0);
		unsigned char* e=s->put(ETH_HLEN+ARP_PLEN);
		std::memcpy(e,OUR_MAC,6); std::memcpy(e+6,GW_MAC,6); wr16be(e+12,ETH_P_ARP);
		unsigned char* a=e+ETH_HLEN; wr16be(a,ARP_HTYPE_ETH); wr16be(a+2,ETH_P_IP); a[4]=6;a[5]=4;
		wr16be(a+6,ARP_OP_REPLY); std::memcpy(a+8,GW_MAC,6); wr32be(a+14,ip); std::memcpy(a+18,OUR_MAC,6); wr32be(a+24,g_dev.ip);
		s->dev=&g_dev; ethRx(s);
	}
	clearCap();
}

// Parsed view of a captured TCP segment (skips eth+ip).
struct Seg { uint16_t sport, dport; uint32_t seq, ack; uint8_t flags; int plen; const unsigned char* data; };
static bool parseCap(Seg* o) {
	if (g_capLen < ETH_HLEN+IP_HLEN_MIN+20) return false;
	const unsigned char* ip=g_cap+ETH_HLEN;
	int ihl=(ip[0]&0xf)*4; int iptot=rd16be(ip+2);
	const unsigned char* t=ip+ihl;
	o->sport=rd16be(t); o->dport=rd16be(t+2); o->seq=rd32be(t+4); o->ack=rd32be(t+8);
	int doff=(t[12]>>4)*4; o->flags=t[13]; o->data=t+doff; o->plen=iptot-ihl-doff;
	// validate the TCP checksum end-to-end
	if (inetPseudoChecksum(rd32be(ip+12), rd32be(ip+16), IPPROTO_TCP, t, iptot-ihl) != 0) return false;
	return true;
}

// Feed a TCP segment (from peer:sport to our:dport) into the stack.
static void feedTcp(uint32_t peer, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                    uint8_t flags, const unsigned char* data, int dlen) {
	NetBuf* skb=netbufAlloc(); skb->reserve(0);
	int tlen=20+dlen, tot=IP_HLEN_MIN+tlen;
	unsigned char* h=skb->put(tot); std::memset(h,0,tot);
	h[0]=0x45; wr16be(h+2,tot); h[8]=64; h[9]=IPPROTO_TCP; wr32be(h+12,peer); wr32be(h+16,g_dev.ip);
	wr16be(h+10,inetChecksum(h,IP_HLEN_MIN));
	unsigned char* t=h+IP_HLEN_MIN;
	wr16be(t,sport); wr16be(t+2,dport); wr32be(t+4,seq); wr32be(t+8,ack);
	t[12]=(20/4)<<4; t[13]=flags; wr16be(t+14,4096); wr16be(t+16,0);
	if (dlen) std::memcpy(t+20,data,dlen);
	wr16be(t+16, inetPseudoChecksum(peer, g_dev.ip, IPPROTO_TCP, t, tlen));
	skb->dev=&g_dev; ipRx(skb);
}

// ---- the active-open handshake, reused by data/close tests. Returns the connected socket. ----
struct Conn { Socket* s; uint16_t lport; uint32_t iss; uint32_t peerSeq; };
static Conn establish(uint32_t peer, uint16_t dport) {
	Conn c; c.s=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	REQUIRE(c.s);
	CHECK(socketConnect(c.s, peer, dport) == 0);
	Seg syn; REQUIRE(parseCap(&syn));
	CHECK((syn.flags & TCP_SYN) != 0);
	c.lport=syn.sport; c.iss=syn.seq;
	c.peerSeq=0x50000;
	clearCap();
	feedTcp(peer, dport, c.lport, c.peerSeq, c.iss+1, TCP_SYN|TCP_ACK, nullptr, 0);  // SYN-ACK
	Seg ack; REQUIRE(parseCap(&ack));                                                 // our ACK
	CHECK((ack.flags & TCP_ACK) != 0);
	CHECK(ack.ack == c.peerSeq+1);
	CHECK(tcpState(c.s) == TCP_ESTABLISHED);
	c.peerSeq += 1;
	clearCap();
	return c;
}

// ---------------------------------------------------------------------------

TEST_CASE("active open: SYN -> SYN-ACK -> ACK, ESTABLISHED") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	CHECK(c.s != nullptr);
	socketClose(c.s);
}

TEST_CASE("data send: segment carries the bytes; ACK advances the window") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	const char* req="GET / HTTP/1.0\r\n\r\n";
	int n=tcpSend(c.s, req, 18);
	CHECK(n==18);
	Seg seg; REQUIRE(parseCap(&seg));
	CHECK((seg.flags & TCP_PSH) != 0);
	CHECK(seg.seq == c.iss+1);
	CHECK(seg.plen == 18);
	CHECK(std::memcmp(seg.data, req, 18)==0);
	// Peer ACKs the data.
	feedTcp(peer, 80, c.lport, c.peerSeq, c.iss+1+18, TCP_ACK, nullptr, 0);
	socketClose(c.s);
}

TEST_CASE("data receive: in-order segment delivered + ACKed") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	const char* resp="HTTP/1.0 200 OK\r\n";
	feedTcp(peer, 80, c.lport, c.peerSeq, c.iss+1, TCP_ACK|TCP_PSH, (const unsigned char*)resp, 17);
	Seg ack; REQUIRE(parseCap(&ack));
	CHECK((ack.flags & TCP_ACK) != 0);
	CHECK(ack.ack == c.peerSeq+17);            // acked the received bytes
	char buf[64]; int n=tcpRecv(c.s, buf, sizeof(buf), 0);
	CHECK(n==17);
	CHECK(std::memcmp(buf, resp, 17)==0);
	socketClose(c.s);
}

TEST_CASE("out-of-order receive: reassembled in sequence") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	unsigned char a[4]={'A','A','A','A'}, b[4]={'B','B','B','B'};
	// Send the SECOND segment (seq+4) first -> queued, not yet delivered.
	feedTcp(peer, 80, c.lport, c.peerSeq+4, c.iss+1, TCP_ACK, b, 4);
	char buf[16];
	CHECK(tcpRecv(c.s, buf, sizeof(buf), 0) == -SOCK_EAGAIN);   // nothing in order yet
	// Now the first segment -> both become available in order.
	feedTcp(peer, 80, c.lport, c.peerSeq, c.iss+1, TCP_ACK, a, 4);
	int n=tcpRecv(c.s, buf, sizeof(buf), 0);
	CHECK(n==8);
	CHECK(std::memcmp(buf, "AAAABBBB", 8)==0);
	socketClose(c.s);
}

TEST_CASE("active close: FIN -> FIN_WAIT_2 -> peer FIN -> TIME_WAIT -> CLOSED") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	socketClose(c.s);                          // last ref -> sends FIN, orphans the TCB
	Seg fin; REQUIRE(parseCap(&fin));
	CHECK((fin.flags & TCP_FIN) != 0);
	uint32_t finSeq=fin.seq;
	clearCap();
	feedTcp(peer, 80, c.lport, c.peerSeq, finSeq+1, TCP_ACK, nullptr, 0);          // ACK of our FIN -> FIN_WAIT_2
	feedTcp(peer, 80, c.lport, c.peerSeq, finSeq+1, TCP_ACK|TCP_FIN, nullptr, 0);  // peer FIN
	Seg ack; REQUIRE(parseCap(&ack));
	CHECK((ack.flags & TCP_ACK) != 0);
	CHECK(ack.ack == c.peerSeq+1);             // ack the peer's FIN
	// Advance past 2*MSL -> the TIME_WAIT TCB is reaped (no crash, no leak).
	g_now += 61000; tcpTick(g_now);
	CHECK(true);
}

TEST_CASE("retransmission: unacked data is resent after RTO") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	const char* d="hello";
	tcpSend(c.s, d, 5);
	Seg first; REQUIRE(parseCap(&first));
	CHECK(first.plen==5);
	uint32_t seq0=first.seq;
	clearCap();
	// No ACK arrives; advance past the RTO -> the segment is retransmitted with the same seq.
	g_now += 1500; tcpTick(g_now);
	Seg rexmit; REQUIRE(parseCap(&rexmit));
	CHECK(rexmit.seq == seq0);
	CHECK(rexmit.plen == 5);
	socketClose(c.s);
}

TEST_CASE("SYN retransmit: no SYN-ACK -> SYN resent after RTO; gives up with ETIMEDOUT") {
	setup();
	uint32_t peer = ipv4(212,77,98,9);
	Socket* s = socketCreate(AF_INET, SOCK_STREAM, 0, nullptr);
	REQUIRE(s);
	CHECK(socketConnect(s, peer, 80) == 0);
	Seg syn1; REQUIRE(parseCap(&syn1));
	CHECK((syn1.flags & TCP_SYN) != 0);
	CHECK((syn1.flags & TCP_ACK) == 0);
	uint32_t iss = syn1.seq; uint16_t lport = syn1.sport;
	clearCap();

	// No SYN-ACK. Advancing past the RTO must RETRANSMIT the SYN (same ISS, still SYN_SENT) —
	// before this fix the RTO timer fired but sent nothing, so a slow/lost SYN-ACK hung forever.
	g_now += 1500; tcpTick(g_now);
	Seg syn2; REQUIRE(parseCap(&syn2));
	CHECK((syn2.flags & TCP_SYN) != 0);
	CHECK(syn2.seq == iss);
	CHECK(syn2.sport == lport);
	CHECK(tcpState(s) == TCP_SYN_SENT);
	clearCap();

	// Keep timing out: after the retry limit the connect aborts — state leaves SYN_SENT and the
	// socket reports ETIMEDOUT (so a blocked connect() returns instead of hanging indefinitely).
	for (int i = 0; i < 10; i++) { g_now += 120000; tcpTick(g_now); }
	CHECK(tcpState(s) != TCP_SYN_SENT);
	CHECK(s->soError == SOCK_ETIMEDOUT);
	socketClose(s);
}

TEST_CASE("segment to a closed port -> RST") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	// No socket bound to 9999; a bare ACK must be answered with RST.
	feedTcp(peer, 12345, 9999, 1000, 2000, TCP_ACK, nullptr, 0);
	Seg rst; REQUIRE(parseCap(&rst));
	CHECK((rst.flags & TCP_RST) != 0);
	CHECK(rst.seq == 2000);                    // RST seq = the offending ACK number
}

TEST_CASE("peer FIN in ESTABLISHED -> CLOSE_WAIT, recv returns EOF, app close -> LAST_ACK") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	feedTcp(peer, 80, c.lport, c.peerSeq, c.iss+1, TCP_ACK|TCP_FIN, nullptr, 0);   // peer closes
	CHECK(tcpState(c.s) == TCP_CLOSE_WAIT);
	char buf[8];
	CHECK(tcpRecv(c.s, buf, sizeof(buf), 0) == 0);     // EOF (peer FIN, no buffered data)
	CHECK(tcpWritable(c.s) == true);                   // half-open: we can still send
	clearCap();
	socketClose(c.s);                                  // our FIN -> LAST_ACK
	Seg fin; REQUIRE(parseCap(&fin));
	CHECK((fin.flags & TCP_FIN) != 0);
	feedTcp(peer, 80, c.lport, c.peerSeq+1, fin.seq+1, TCP_ACK, nullptr, 0);   // ACK -> CLOSED (TCB freed)
	CHECK(true);
}

TEST_CASE("RST aborts the connection (SO_ERROR = ECONNREFUSED)") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	feedTcp(peer, 80, c.lport, c.peerSeq, c.iss+1, TCP_RST, nullptr, 0);
	CHECK(tcpState(c.s) == TCP_CLOSED);
	CHECK(c.s->soError == SOCK_ECONNREFUSED);
	socketClose(c.s);
}

TEST_CASE("MSG_PEEK on a stream leaves the bytes; recv then consumes") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	feedTcp(peer, 80, c.lport, c.peerSeq, c.iss+1, TCP_ACK|TCP_PSH, (const unsigned char*)"abcde", 5);
	char buf[8];
	CHECK(tcpRecv(c.s, buf, sizeof(buf), MSG_PEEK) == 5);
	CHECK(tcpRecv(c.s, buf, sizeof(buf), 0) == 5);      // still there after peek
	CHECK(std::memcmp(buf, "abcde", 5)==0);
	CHECK(tcpRecv(c.s, buf, sizeof(buf), 0) == -SOCK_EAGAIN);
	socketClose(c.s);
}

TEST_CASE("fast retransmit on 3 duplicate ACKs") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	Conn c=establish(peer, 80);
	tcpSend(c.s, "0123456789", 10);
	Seg seg; REQUIRE(parseCap(&seg));
	uint32_t seq0=seg.seq;
	clearCap();
	// Three duplicate ACKs (acking only iss+1, i.e. no progress) -> fast retransmit.
	for (int i=0;i<3;i++) feedTcp(peer, 80, c.lport, c.peerSeq, c.iss+1, TCP_ACK, nullptr, 0);
	Seg rx; REQUIRE(parseCap(&rx));
	CHECK(rx.seq == seq0);            // retransmitted from snd_una
	CHECK(rx.plen == 10);
	socketClose(c.s);
}

TEST_CASE("bare SYN to a closed port -> RST+ACK; send-buffer fills -> EAGAIN") {
	setup();
	uint32_t peer=ipv4(212,77,98,9);
	feedTcp(peer, 30000, 7777, 0x123, 0, TCP_SYN, nullptr, 0);   // no listener
	Seg rst; REQUIRE(parseCap(&rst));
	CHECK((rst.flags & (TCP_RST|TCP_ACK)) == (TCP_RST|TCP_ACK));
	CHECK(rst.ack == 0x124);          // ack the SYN (seq+1)
	// send-buffer back-pressure: fill it past capacity -> short write / EAGAIN.
	Conn c=establish(peer, 80);
	static char big[20000];
	int total=0, n;
	while ((n=tcpSend(c.s, big, sizeof(big))) > 0) total += n;
	CHECK(n == -SOCK_EAGAIN);         // buffer full
	CHECK(total > 0);
	socketClose(c.s);
}

TEST_CASE("passive open: LISTEN -> SYN-ACK -> ACK, accept() returns a connection") {
	setup();
	uint32_t peer=ipv4(10,0,2,80);
	Socket* srv=socketCreate(AF_INET,SOCK_STREAM,0,nullptr);
	socketBind(srv, 0, 8080);
	CHECK(tcpListen(srv, 8) == 0);
	clearCap();
	feedTcp(peer, 40000, 8080, 0x900, 0, TCP_SYN, nullptr, 0);     // incoming SYN
	Seg synack; REQUIRE(parseCap(&synack));
	CHECK((synack.flags & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK));
	CHECK(synack.ack == 0x901);
	uint32_t srvIss=synack.seq;
	feedTcp(peer, 40000, 8080, 0x901, srvIss+1, TCP_ACK, nullptr, 0);   // handshake completes
	int err=-1;
	Socket* conn=tcpAccept(srv, &err);
	REQUIRE(conn != nullptr);
	CHECK(err==0);
	CHECK(tcpState(conn) == TCP_ESTABLISHED);
	socketClose(conn); socketClose(srv);
}
