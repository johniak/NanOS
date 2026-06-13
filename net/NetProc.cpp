/*
 * NetProc.cpp — Linux-format /proc/net renderers (see NetProc.h). Freestanding string building
 * (no snprintf): a cursor `p` bounded by `end` that all helpers respect, so output is truncated
 * safely if the buffer is small. State is read live from the net modules' introspection APIs.
 */
#include "NetProc.h"
#include "Net.h"
#include "NetDevice.h"
#include "Arp.h"
#include "Route.h"
#include "Socket.h"
#include "Tcp.h"
#include "Unix.h"
#include "NetStats.h"

namespace kernel {
namespace {

// ---- bounded append helpers ----
void apC(char*& p, char* end, char c) { if (p < end) *p++ = c; }
void apS(char*& p, char* end, const char* s) { while (*s) apC(p, end, *s++); }
void apPad(char*& p, char* end, int n) { while (n-- > 0) apC(p, end, ' '); }

// Unsigned decimal. Uses `unsigned long` (32-bit on i686 — avoids libgcc's __udivdi3, which the
// freestanding kernel doesn't link; 64-bit on the LP64 test host). Counters wider than 2^32 are
// truncated for display, which is fine for these introspection files.
void apU(char*& p, char* end, unsigned long v) {
	char t[24]; int n = 0;
	do { t[n++] = (char) ('0' + (v % 10)); v /= 10; } while (v && n < 24);
	while (n) apC(p, end, t[--n]);
}
// Zero-padded hex of `v` to exactly `width` digits (upper-case).
void apHexW(char*& p, char* end, unsigned long long v, int width) {
	char t[16]; int n = 0;
	do { int d = (int) (v & 0xf); t[n++] = (char) (d < 10 ? '0' + d : 'A' + d - 10); v >>= 4; } while (v && n < 16);
	for (int i = n; i < width; i++) apC(p, end, '0');
	while (n) apC(p, end, t[--n]);
}
// Right-justify a decimal in a field of `width` (32-bit, see apU).
void apURight(char*& p, char* end, unsigned long v, int width) {
	char t[24]; int n = 0;
	do { t[n++] = (char) ('0' + (v % 10)); v /= 10; } while (v && n < 24);
	for (int i = n; i < width; i++) apC(p, end, ' ');
	while (n) apC(p, end, t[--n]);
}
// Dotted-decimal IPv4 from a host-order address.
void apIp(char*& p, char* end, uint32_t ip) {
	apU(p, end, (ip >> 24) & 0xff); apC(p, end, '.');
	apU(p, end, (ip >> 16) & 0xff); apC(p, end, '.');
	apU(p, end, (ip >> 8) & 0xff);  apC(p, end, '.');
	apU(p, end, ip & 0xff);
}
// MAC as xx:xx:xx:xx:xx:xx (lower-case, like /proc/net/arp).
void apMac(char*& p, char* end, const unsigned char* mac) {
	const char* hx = "0123456789abcdef";
	for (int i = 0; i < 6; i++) {
		if (i) apC(p, end, ':');
		apC(p, end, hx[(mac[i] >> 4) & 0xf]); apC(p, end, hx[mac[i] & 0xf]);
	}
}
// Linux /proc address column: IPv4 (host order) printed as its network-order bytes read as a
// little-endian word -> %08X. On any host that is hton32(ip). Port as %04X (host order).
void apProcAddr(char*& p, char* end, uint32_t ip, uint16_t port) {
	apHexW(p, end, hton32(ip), 8); apC(p, end, ':'); apHexW(p, end, port, 4);
}

// Our TcpState enum -> Linux /proc/net/tcp `st` numbering (include/net/tcp_states.h).
int linuxTcpSt(int st) {
	switch (st) {
	case TCP_ESTABLISHED: return 0x01;
	case TCP_SYN_SENT:    return 0x02;
	case TCP_SYN_RCVD:    return 0x03;
	case TCP_FIN_WAIT_1:  return 0x04;
	case TCP_FIN_WAIT_2:  return 0x05;
	case TCP_TIME_WAIT:   return 0x06;
	case TCP_CLOSED:      return 0x07;
	case TCP_CLOSE_WAIT:  return 0x08;
	case TCP_LAST_ACK:    return 0x09;
	case TCP_LISTEN:      return 0x0A;
	case TCP_CLOSING:     return 0x0B;
	default:              return 0x07;
	}
}

// One /proc/net/tcp-style socket line (used by tcp/udp/raw). `sl` index, addresses host order,
// `st` already in Linux numbering, queues in bytes. Matches Linux column layout closely enough
// for tools (ss/netstat read the fields positionally).
void apSockLine(char*& p, char* end, int sl, uint32_t lip, uint16_t lport,
                uint32_t rip, uint16_t rport, int st, unsigned txq, unsigned rxq) {
	apURight(p, end, (unsigned) sl, 4); apC(p, end, ':'); apC(p, end, ' ');
	apProcAddr(p, end, lip, lport); apC(p, end, ' ');
	apProcAddr(p, end, rip, rport); apC(p, end, ' ');
	apHexW(p, end, (unsigned) st, 2); apC(p, end, ' ');
	apHexW(p, end, txq, 8); apC(p, end, ':'); apHexW(p, end, rxq, 8); apC(p, end, ' ');
	apS(p, end, "00:00000000 00000000     0        0 0\n");
}

}  // namespace

int netProcDev(char* buf, int cap) {
	char* p = buf; char* end = buf + cap;
	apS(p, end, "Inter-|   Receive                                                |  Transmit\n");
	apS(p, end, " face |bytes    packets errs drop fifo frame compressed multicast|"
	            "bytes    packets errs drop fifo colls carrier compressed\n");
	for (int i = 0; i < netCount(); i++) {
		NetDevice* d = netByIndex(i);
		if (!d) continue;
		// name right-justified in a 6-col field, then ':'
		int nl = 0; while (d->name[nl]) nl++;
		apPad(p, end, nl < 6 ? 6 - nl : 0);
		apS(p, end, d->name); apC(p, end, ':');
		apURight(p, end, d->rxBytes, 8); apC(p, end, ' ');
		apURight(p, end, d->rxPackets, 7); apC(p, end, ' ');
		apURight(p, end, d->rxErrors, 4); apC(p, end, ' ');
		apURight(p, end, d->rxDropped, 4); apS(p, end, "    0     0          0         0 ");
		apURight(p, end, d->txBytes, 8); apC(p, end, ' ');
		apURight(p, end, d->txPackets, 7); apC(p, end, ' ');
		apURight(p, end, d->txErrors, 4); apC(p, end, ' ');
		apURight(p, end, d->txDropped, 4); apS(p, end, "    0     0       0          0\n");
	}
	return (int) (p - buf);
}

int netProcRoute(char* buf, int cap) {
	char* p = buf; char* end = buf + cap;
	apS(p, end, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
	for (int i = 0; i < routeCount(); i++) {
		const Route* r = routeByIndex(i);
		if (!r || !r->dev) continue;
		int flags = 0x0001;                 // RTF_UP
		if (r->gw) flags |= 0x0002;         // RTF_GATEWAY
		apS(p, end, r->dev->name); apC(p, end, '\t');
		apHexW(p, end, hton32(r->dest), 8); apC(p, end, '\t');
		apHexW(p, end, hton32(r->gw), 8); apC(p, end, '\t');
		apHexW(p, end, (unsigned) flags, 4); apC(p, end, '\t');
		apS(p, end, "0\t0\t"); apU(p, end, (unsigned) r->metric); apC(p, end, '\t');
		apHexW(p, end, hton32(r->mask), 8); apS(p, end, "\t0\t0\t0\n");
	}
	return (int) (p - buf);
}

int netProcArp(char* buf, int cap) {
	char* p = buf; char* end = buf + cap;
	apS(p, end, "IP address       HW type     Flags       HW address            Mask     Device\n");
	for (int i = 0; i < arpSlots(); i++) {
		const ArpEntry* e = arpEntryAt(i);
		if (!e || e->state == ARP_FREE) continue;
		NetDevice* dev = netPrimary();      // entries don't store their dev; report the primary
		char ips[20]; char* q = ips; char* qe = ips + sizeof(ips);
		apIp(q, qe, e->ip); *q = 0;
		int il = (int) (q - ips);
		apS(p, end, ips); apPad(p, end, il < 16 ? 16 - il : 1); apC(p, end, ' ');
		apS(p, end, "0x1         ");                                  // HW type = ARPHRD_ETHER
		// Flags: 0x2 = ATF_COM (resolved); 0x0 while INCOMPLETE.
		apS(p, end, e->state == ARP_INCOMPLETE ? "0x0         " : "0x2         ");
		apMac(p, end, e->mac); apS(p, end, "     *        ");
		apS(p, end, dev ? dev->name : "eth0"); apC(p, end, '\n');
	}
	return (int) (p - buf);
}

int netProcTcp(char* buf, int cap) {
	char* p = buf; char* end = buf + cap;
	apS(p, end, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
	            "retrnsmt   uid  timeout inode\n");
	TcpConnInfo c[8];
	int n = tcpSnapshot(c, 8);
	for (int i = 0; i < n; i++)
		apSockLine(p, end, i, c[i].localIp, c[i].localPort, c[i].remoteIp, c[i].remotePort,
		           linuxTcpSt(c[i].state), (unsigned) c[i].txQueue, (unsigned) c[i].rxQueue);
	return (int) (p - buf);
}

int netProcUdp(char* buf, int cap) {
	char* p = buf; char* end = buf + cap;
	apS(p, end, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
	            "retrnsmt   uid  timeout inode\n");
	int sl = 0;
	for (int i = 0; i < socketSlots(); i++) {
		Socket* s = socketAt(i);
		if (!s || s->type != SOCK_DGRAM) continue;
		int st = s->connected ? 0x01 : 0x07;   // 01=ESTABLISHED-ish, 07=CLOSE (unconnected)
		apSockLine(p, end, sl++, s->localIp, s->localPort, s->remoteIp, s->remotePort,
		           st, 0u /* UDP: no send queue */, (unsigned) s->rxBytes);
	}
	return (int) (p - buf);
}

// /proc/net/unix — AF_UNIX sockets, Linux column layout (Num RefCount Protocol Flags Type St
// Inode Path). St: 01 unconnected/listening, 03 connected. Flags bit 0x10000 = SO_ACCEPTCON.
int netProcUnix(char* buf, int cap) {
	char* p = buf; char* end = buf + cap;
	apS(p, end, "Num       RefCount Protocol Flags    Type St Inode Path\n");
	for (int i = 0; i < socketSlots(); i++) {
		Socket* s = socketAt(i);
		if (!s || s->domain != AF_UNIX) continue;
		bool listening = unixIsListening(s);
		bool conn = unixPeerOf(s) != 0 || s->connected;
		apHexW(p, end, (unsigned long) ((i + 1) * 0x100), 8); apC(p, end, ':'); apC(p, end, ' ');
		apHexW(p, end, (unsigned) s->refs, 8); apC(p, end, ' ');
		apHexW(p, end, 0, 8); apC(p, end, ' ');                       // Protocol (always 0)
		apHexW(p, end, listening ? 0x10000u : 0u, 8); apC(p, end, ' ');
		apHexW(p, end, (unsigned) s->type, 4); apC(p, end, ' ');      // 0001 stream / 0002 dgram
		apHexW(p, end, conn ? 0x03u : 0x01u, 2); apC(p, end, ' ');
		apURight(p, end, (unsigned) (i + 1), 5); apC(p, end, ' ');    // Inode
		char path[UNIX_PATH_MAX];
		unsigned pl = unixGetName(s, path, sizeof path);
		if (pl > 0) {
			if (path[0] == 0) { apC(p, end, '@'); for (unsigned k = 1; k < pl && path[k]; k++) apC(p, end, path[k]); }
			else for (unsigned k = 0; k < pl && path[k]; k++) apC(p, end, path[k]);
		}
		apC(p, end, '\n');
	}
	return (int) (p - buf);
}

int netProcRaw(char* buf, int cap) {
	char* p = buf; char* end = buf + cap;
	apS(p, end, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
	            "retrnsmt   uid  timeout inode\n");
	int sl = 0;
	for (int i = 0; i < socketSlots(); i++) {
		Socket* s = socketAt(i);
		if (!s || s->type != SOCK_RAW) continue;
		// Linux puts the protocol number where the local port goes for raw sockets.
		apSockLine(p, end, sl++, s->localIp, (uint16_t) s->protocol, s->remoteIp, 0,
		           s->connected ? 0x01 : 0x07, 0u, (unsigned) s->rxBytes);
	}
	return (int) (p - buf);
}

int netProcSnmp(char* buf, int cap) {
	char* p = buf; char* end = buf + cap;
	const NetStats& s = g_netStats;
	// IP (Forwarding=2 disabled, DefaultTTL=64). Fields we don't count are 0.
	apS(p, end, "Ip: Forwarding DefaultTTL InReceives InHdrErrors InAddrErrors ForwDatagrams "
	            "InUnknownProtos InDiscards InDelivers OutRequests OutDiscards OutNoRoutes "
	            "ReasmTimeout ReasmReqds ReasmOKs ReasmFails FragOKs FragFails FragCreates\n");
	apS(p, end, "Ip: 2 64 ");
	apU(p, end, s.ipInReceives); apC(p, end, ' '); apU(p, end, s.ipInHdrErrors); apC(p, end, ' ');
	apU(p, end, s.ipInAddrErrors); apS(p, end, " 0 0 0 ");
	apU(p, end, s.ipInDelivers); apC(p, end, ' '); apU(p, end, s.ipOutRequests); apS(p, end, " 0 ");
	apU(p, end, s.ipOutNoRoutes); apS(p, end, " 0 ");
	apU(p, end, s.ipReasmReqds); apC(p, end, ' '); apU(p, end, s.ipReasmOKs); apC(p, end, ' ');
	apU(p, end, s.ipReasmFails); apC(p, end, ' '); apU(p, end, s.ipFragOKs); apC(p, end, ' ');
	apU(p, end, s.ipFragFails); apC(p, end, ' '); apU(p, end, s.ipFragCreates); apC(p, end, '\n');
	// ICMP
	apS(p, end, "Icmp: InMsgs InErrors InCsumErrors InDestUnreachs InTimeExcds InParmProbs "
	            "InSrcQuenchs InRedirects InEchos InEchoReps InTimestamps InTimestampReps "
	            "InAddrMasks InAddrMaskReps OutMsgs OutErrors OutDestUnreachs OutTimeExcds "
	            "OutParmProbs OutSrcQuenchs OutRedirects OutEchos OutEchoReps OutTimestamps "
	            "OutTimestampReps OutAddrMasks OutAddrMaskReps\n");
	apS(p, end, "Icmp: ");
	apU(p, end, s.icmpInMsgs); apC(p, end, ' '); apU(p, end, s.icmpInErrors); apS(p, end, " 0 ");
	apU(p, end, s.icmpInDestUnreachs); apS(p, end, " 0 0 0 0 ");
	apU(p, end, s.icmpInEchos); apC(p, end, ' '); apU(p, end, s.icmpInEchoReps); apS(p, end, " 0 0 0 0 ");
	apU(p, end, s.icmpOutMsgs); apC(p, end, ' '); apU(p, end, s.icmpOutErrors); apC(p, end, ' ');
	apU(p, end, s.icmpOutDestUnreachs); apS(p, end, " 0 0 0 0 ");
	apU(p, end, s.icmpOutEchos); apC(p, end, ' '); apU(p, end, s.icmpOutEchoReps); apS(p, end, " 0 0 0 0\n");
	// TCP (RtoAlgorithm=1 (other), RtoMin=200, RtoMax=120000, MaxConn=-1)
	apS(p, end, "Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails "
	            "EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors\n");
	apS(p, end, "Tcp: 1 200 120000 -1 ");
	apU(p, end, s.tcpActiveOpens); apC(p, end, ' '); apU(p, end, s.tcpPassiveOpens); apC(p, end, ' ');
	apU(p, end, s.tcpAttemptFails); apC(p, end, ' '); apU(p, end, s.tcpEstabResets); apS(p, end, " 0 ");
	apU(p, end, s.tcpInSegs); apC(p, end, ' '); apU(p, end, s.tcpOutSegs); apC(p, end, ' ');
	apU(p, end, s.tcpRetransSegs); apC(p, end, ' '); apU(p, end, s.tcpInErrs); apC(p, end, ' ');
	apU(p, end, s.tcpOutRsts); apS(p, end, " 0\n");
	// UDP
	apS(p, end, "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors "
	            "InCsumErrors IgnoredMulti\n");
	apS(p, end, "Udp: ");
	apU(p, end, s.udpInDatagrams); apC(p, end, ' '); apU(p, end, s.udpNoPorts); apC(p, end, ' ');
	apU(p, end, s.udpInErrors); apC(p, end, ' '); apU(p, end, s.udpOutDatagrams); apS(p, end, " 0 0 0 0\n");
	return (int) (p - buf);
}

}  // namespace kernel
