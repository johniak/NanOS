#include "Icmp.h"
#include "Ip.h"
#include "Ether.h"   // NET_HEADROOM
#include "NetBuf.h"
#include "Net.h"
#include "NetStats.h"   // /proc/net/snmp counters
#include "Socket.h"     // SOCK_E* errno codes (ICMP error -> socket)
#include "Udp.h"        // udpIcmpError
#include "Tcp.h"        // tcpIcmpError
#include <string.h>

namespace kernel {

namespace {
IcmpRawFn       g_raw = 0;
IcmpEchoReplyFn g_echoReply = 0;

// Build an ICMP message (type/code + 4 header bytes + body) into a fresh skb and ipOutput it.
// hdr4 are the 4 bytes after the checksum (id/seq for echo, unused for errors).
int icmpEmit(uint32_t dst, uint8_t type, uint8_t code, const unsigned char hdr4[4],
             const void* body, int bodyLen) {
	NetBuf* skb = netbufAlloc();
	if (!skb) return -1;
	skb->reserve(NET_HEADROOM);
	unsigned char* m = skb->put(8 + bodyLen);
	m[0] = type; m[1] = code;
	wr16be(m + 2, 0);                          // checksum (computed below)
	memcpy(m + 4, hdr4, 4);
	if (bodyLen > 0 && body) memcpy(m + 8, body, bodyLen);
	wr16be(m + 2, inetChecksum(m, 8 + bodyLen));
	g_netStats.icmpOutMsgs++;
	if (type == ICMP_ECHO_REQUEST)      g_netStats.icmpOutEchos++;
	else if (type == ICMP_ECHO_REPLY)   g_netStats.icmpOutEchoReps++;
	else if (type == ICMP_DEST_UNREACH) g_netStats.icmpOutDestUnreachs++;
	return ipOutput(dst, IPPROTO_ICMP, skb);
}
}  // namespace

void icmpSetRawHandler(IcmpRawFn fn)        { g_raw = fn; }
void icmpSetEchoReplyHandler(IcmpEchoReplyFn fn) { g_echoReply = fn; }

// Map an ICMP dest-unreachable code (and the soft errors) to the errno a socket reports, exactly
// like Linux's icmp_err_convert for the cases we deliver.
static int icmpErrToErrno(uint8_t type, uint8_t code) {
	if (type == ICMP_DEST_UNREACH) {
		switch (code) {
			case ICMP_NET_UNREACH:  return SOCK_ENETUNREACH;
			case ICMP_HOST_UNREACH: return SOCK_EHOSTUNREACH;
			case ICMP_PORT_UNREACH: return SOCK_ECONNREFUSED;
			case ICMP_FRAG_NEEDED:  return SOCK_EMSGSIZE;
			default:                return SOCK_EHOSTUNREACH;
		}
	}
	// time-exceeded / parameter-problem: a soft error. A connected socket sees EHOSTUNREACH;
	// traceroute reads the router identity from its RAW socket (which still gets the packet).
	return SOCK_EHOSTUNREACH;
}

// An ICMP error (type 3/11/12) quotes the offending IP header + its first 8 bytes (RFC 792). Parse
// the quote, extract the 4-tuple of OUR outgoing datagram (src/ports), and hand the mapped errno to
// the transport so it can wake the matching socket. Validates the quote and ignores anything short.
static void icmpDeliverError(NetBuf* skb, uint8_t type, uint8_t code) {
	const unsigned char* m = skb->head();
	int qlen = (int) skb->len - 8;                    // bytes after type/code/csum/unused(4)
	if (qlen < IP_HLEN_MIN) return;                   // no quoted IP header at all
	const unsigned char* q = m + 8;
	int ihl = (q[0] & 0x0f) * 4;
	if (ihl < IP_HLEN_MIN || qlen < ihl + 8) return;  // need the IP header + 8 transport bytes
	uint8_t proto  = q[9];
	uint32_t qSrc  = rd32be(q + 12);                  // our outgoing src = local IP
	uint32_t qDst  = rd32be(q + 16);                  // our outgoing dst = remote IP
	const unsigned char* l4 = q + ihl;
	uint16_t sport = rd16be(l4 + 0);                  // our local port
	uint16_t dport = rd16be(l4 + 2);                  // remote port
	int err = icmpErrToErrno(type, code);
	if (proto == IPPROTO_UDP)      udpIcmpError(qSrc, sport, qDst, dport, err);
	else if (proto == IPPROTO_TCP) tcpIcmpError(qSrc, sport, qDst, dport, err);
}

void icmpInit() { ipSetHandler(IPPROTO_ICMP, icmpRx); }

void icmpRx(NetBuf* skb) {
	if (!skb) return;
	if (skb->len < 8) { g_netStats.icmpInMsgs++; g_netStats.icmpInErrors++; netbufFree(skb); return; }
	const unsigned char* m = skb->head();
	if (inetChecksum(m, skb->len) != 0) { g_netStats.icmpInMsgs++; g_netStats.icmpInErrors++; netbufFree(skb); return; }
	uint8_t type = m[0];
	g_netStats.icmpInMsgs++;
	if (type == ICMP_ECHO_REQUEST)      g_netStats.icmpInEchos++;
	else if (type == ICMP_ECHO_REPLY)   g_netStats.icmpInEchoReps++;
	else if (type == ICMP_DEST_UNREACH) g_netStats.icmpInDestUnreachs++;

	// Auto-reply to echo requests (kernel behaviour, independent of raw sockets): swap to a
	// reply, keep id/seq/data, send back to the sender. Build a new skb so the original can also
	// go to a raw socket.
	if (type == ICMP_ECHO_REQUEST) {
		unsigned char hdr4[4]; memcpy(hdr4, m + 4, 4);                 // id + seq
		int bodyLen = skb->len - 8;
		icmpEmit(skb->saddr, ICMP_ECHO_REPLY, 0, hdr4, m + 8, bodyLen);
	} else if (type == ICMP_ECHO_REPLY && g_echoReply && !g_raw) {
		g_echoReply(skb->saddr, rd16be(m + 4), rd16be(m + 6));
	} else if (type == ICMP_DEST_UNREACH || type == ICMP_TIME_EXCEEDED || type == ICMP_PARAM_PROBLEM) {
		// Deliver the error to the connected UDP/TCP socket it refers to (Linux sk_err). Raw
		// sockets below still receive the full message (traceroute reads the router from there).
		icmpDeliverError(skb, type, m[1]);
	}

	if (g_raw) g_raw(skb);     // SOCK_RAW sockets see all ICMP (FAZA 7); raw owns the skb
	else       netbufFree(skb);
}

int icmpSendEcho(uint32_t dst, uint16_t id, uint16_t seq, const void* data, int len) {
	unsigned char hdr4[4];
	wr16be(hdr4, id);
	wr16be(hdr4 + 2, seq);
	return icmpEmit(dst, ICMP_ECHO_REQUEST, 0, hdr4, data, len);
}

void icmpSendError(NetBuf* orig, uint8_t type, uint8_t code) {
	if (!orig || orig->l3 < 0) return;
	// Quote the offending IP header + first 8 bytes of its payload (RFC 792). l3/l4 were set by
	// the IP demux before the header was pulled, so the IP header is still in the buffer.
	const unsigned char* ipHdr = orig->buf + orig->l3;
	int ihl = (orig->l4 >= 0) ? (orig->l4 - orig->l3) : IP_HLEN_MIN;
	int avail = orig->data + orig->len - orig->l3;     // bytes from the IP header to the tail
	int quote = ihl + 8;
	if (quote > avail) quote = avail;
	if (quote < 0) return;
	unsigned char unused[4] = { 0, 0, 0, 0 };
	icmpEmit(orig->saddr, type, code, unused, ipHdr, quote);
}

void icmpReset() { g_raw = 0; g_echoReply = 0; }

}  // namespace kernel
