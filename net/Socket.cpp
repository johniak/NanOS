#include "Socket.h"
#include "NetBuf.h"
#include "Ip.h"       // IPPROTO_* for RAW protocol validation
#include "Tcp.h"      // SOCK_STREAM dispatch
#include <string.h>

namespace kernel {

const int Socket::RXQ;   // out-of-class definition (ODR-used, e.g. by doctest's CHECK by-ref)

// Forward decls of the transport senders (net/Udp.cpp, net/Raw.cpp). Same MI layer; resolved at link.
int udpSend(Socket* s, const void* buf, unsigned len, uint32_t dstIp, uint16_t dstPort);
int rawSend(Socket* s, const void* buf, unsigned len, uint32_t dstIp, uint16_t dstPort);

namespace {
const int SOCK_N = 64;
Socket g_socks[SOCK_N];
SocketWakeFn g_wake = 0;
uint16_t g_nextEphemeral = 32768;

enum { POLLIN = 0x001, POLLOUT = 0x004, POLLERR = 0x008 };
const int DEFAULT_BUF = 64 * 1024;
}  // namespace

void socketSetWakeFn(SocketWakeFn fn) { g_wake = fn; }
void socketWakeReaders(Socket* s) { if (s && g_wake) g_wake(&s->rxWait); }

Socket* socketCreate(int domain, int type, int protocol, int* err) {
	if (domain == AF_INET) {
		if (type != SOCK_DGRAM && type != SOCK_RAW && type != SOCK_STREAM) {
			if (err) *err = -SOCK_EPROTONOSUPPORT;
			return 0;
		}
	} else {
		// AF_INET6 -> EAFNOSUPPORT (IPv4-only stack, FAZA 11 semantics); AF_PACKET -> FAZA 10.
		if (err) *err = -SOCK_EAFNOSUPPORT;
		return 0;
	}
	for (int i = 0; i < SOCK_N; i++) {
		if (!g_socks[i].used) {
			Socket* s = &g_socks[i];
			*s = Socket{};               // value-init (zeroes PODs + default-constructs the WaitQueue)
			s->used = true; s->refs = 1;
			s->domain = domain; s->type = type; s->protocol = protocol;
			s->rcvbuf = DEFAULT_BUF; s->sndbuf = DEFAULT_BUF;
			s->rxHead = s->rxTail = s->rxCount = s->rxBytes = 0;
			if (type == SOCK_STREAM) {
				int rc = tcpAttach(s);                  // give it a TCB
				if (rc < 0) { s->used = false; if (err) *err = rc; return 0; }
			}
			if (err) *err = 0;
			return s;
		}
	}
	if (err) *err = -SOCK_ENOBUFS;
	return 0;
}

Socket* socketCreateRaw(int domain, int type, int protocol) { return socketCreate(domain, type, protocol, 0); }

void socketRef(Socket* s) { if (s) s->refs++; }

void socketClose(Socket* s) {
	if (!s) return;
	if (--s->refs > 0) return;
	if (s->type == SOCK_STREAM && s->tcp)    // begin the TCP close handshake (orphans the TCB)
		tcpClose(s);
	while (s->rxCount > 0) {                 // free any queued datagrams
		netbufFree(s->rxq[s->rxTail].skb);
		s->rxTail = (s->rxTail + 1) % Socket::RXQ;
		s->rxCount--;
	}
	s->used = false;
}

static bool portInUseUdp(uint16_t port, Socket* except) {
	for (int i = 0; i < SOCK_N; i++) {
		Socket* s = &g_socks[i];
		if (s->used && s != except && s->type == SOCK_DGRAM && s->bound && s->localPort == port)
			return true;
	}
	return false;
}

uint16_t socketEphemeralPort() {
	for (int tries = 0; tries < (60999 - 32768 + 1); tries++) {
		uint16_t p = g_nextEphemeral;
		g_nextEphemeral = (g_nextEphemeral >= 60999) ? 32768 : (uint16_t) (g_nextEphemeral + 1);
		if (!portInUseUdp(p, 0))
			return p;
	}
	return 0;
}

int socketBind(Socket* s, uint32_t ip, uint16_t port) {
	if (!s) return -SOCK_EINVAL;
	if (s->bound) return -SOCK_EINVAL;
	if (s->type == SOCK_DGRAM) {
		if (port == 0) port = socketEphemeralPort();
		else if (portInUseUdp(port, s)) return -SOCK_EADDRINUSE;
	}
	// (STREAM bind just records the local addr/port for listen(); TCP demux disambiguates by 4-tuple.)
	s->localIp = ip; s->localPort = port; s->bound = true;
	return 0;
}

int socketConnect(Socket* s, uint32_t ip, uint16_t port) {
	if (!s) return -SOCK_EINVAL;
	if (s->type == SOCK_STREAM) {            // TCP active open (3-way handshake)
		int rc = tcpConnect(s, ip, port);
		if (rc == 0) { s->remoteIp = ip; s->remotePort = port; s->connected = true; }
		return rc;
	}
	s->remoteIp = ip; s->remotePort = port; s->connected = true;
	if (!s->bound && s->type == SOCK_DGRAM) {     // pick a local port (Linux auto-binds on connect)
		s->localPort = socketEphemeralPort();
		s->bound = true;
	}
	return 0;
}

int socketGetSockName(Socket* s, uint32_t* ip, uint16_t* port) {
	if (!s) return -SOCK_EINVAL;
	if (ip) *ip = s->localIp;
	if (port) *port = s->localPort;
	return 0;
}
int socketGetPeerName(Socket* s, uint32_t* ip, uint16_t* port) {
	if (!s) return -SOCK_EINVAL;
	if (!s->connected) return -SOCK_ENOTCONN;
	if (ip) *ip = s->remoteIp;
	if (port) *port = s->remotePort;
	return 0;
}

int socketSetOpt(Socket* s, int level, int name, const void* val, unsigned len) {
	if (!s || level != SOL_SOCKET || !val || len < sizeof(int)) return -SOCK_EINVAL;
	int v = *(const int*) val;
	switch (name) {
	case SO_BROADCAST: s->broadcast = v != 0; return 0;
	case SO_RCVBUF: s->rcvbuf = v > 0 ? v : s->rcvbuf; return 0;
	case SO_SNDBUF: s->sndbuf = v > 0 ? v : s->sndbuf; return 0;
	case SO_REUSEADDR: return 0;          // accepted; our bind already allows quick reuse
	default: return -SOCK_EINVAL;
	}
}
int socketGetOpt(Socket* s, int level, int name, void* val, unsigned* len) {
	if (!s || level != SOL_SOCKET || !val || !len || *len < sizeof(int)) return -SOCK_EINVAL;
	int v = 0;
	switch (name) {
	case SO_TYPE: v = s->type; break;
	case SO_ERROR: v = s->soError; s->soError = 0; break;   // read-and-clear, like Linux
	case SO_BROADCAST: v = s->broadcast; break;
	case SO_RCVBUF: v = s->rcvbuf; break;
	case SO_SNDBUF: v = s->sndbuf; break;
	default: return -SOCK_EINVAL;
	}
	*(int*) val = v; *len = sizeof(int);
	return 0;
}

int socketSendTo(Socket* s, const void* buf, unsigned len, uint32_t dstIp, uint16_t dstPort) {
	if (!s) return -SOCK_EINVAL;
	if (s->type == SOCK_STREAM) return tcpSend(s, buf, len);    // stream: ignore dst, use the connection
	if (s->connected) { dstIp = s->remoteIp; dstPort = s->remotePort; }
	else if (dstIp == 0) return -SOCK_ENOTCONN;       // unconnected send needs a destination
	if (s->type == SOCK_DGRAM) return udpSend(s, buf, len, dstIp, dstPort);
	if (s->type == SOCK_RAW)   return rawSend(s, buf, len, dstIp, dstPort);
	return -SOCK_EINVAL;
}

int socketRecvFrom(Socket* s, void* buf, unsigned len, uint32_t* srcIp, uint16_t* srcPort, int flags) {
	if (!s) return -SOCK_EINVAL;
	if (s->type == SOCK_STREAM) {            // stream: byte recv from the connection
		if (srcIp) *srcIp = s->remoteIp;
		if (srcPort) *srcPort = s->remotePort;
		return tcpRecv(s, buf, len, flags);
	}
	if (s->rxCount == 0)
		return -SOCK_EAGAIN;                          // dispatch blocks on socketReadable unless nonblock
	Socket::Dgram* d = &s->rxq[s->rxTail];
	int avail = d->skb->len;
	int n = (int) len < avail ? (int) len : avail;    // truncate to the buffer (DGRAM semantics)
	memcpy(buf, d->skb->head(), n);
	if (srcIp) *srcIp = d->srcIp;
	if (srcPort) *srcPort = d->srcPort;
	if (!(flags & MSG_PEEK)) {
		s->rxBytes -= d->skb->len;
		netbufFree(d->skb);
		s->rxTail = (s->rxTail + 1) % Socket::RXQ;
		s->rxCount--;
	}
	return n;
}

bool socketReadable(const Socket* s) {
	if (!s) return false;
	if (s->type == SOCK_STREAM) return tcpReadable((Socket*) s);
	return s->rxCount > 0 || s->soError != 0;
}
bool socketWritable(const Socket* s) {
	if (s && s->type == SOCK_STREAM) return tcpWritable((Socket*) s);
	return s != 0;   // datagram sockets are always writable
}
int socketPoll(Socket* s) {
	int e = 0;
	if (socketReadable(s)) e |= POLLIN;
	if (socketWritable(s)) e |= POLLOUT;
	if (s && s->soError) e |= POLLERR;
	return e;
}

bool socketDeliver(Socket* s, NetBuf* skb, uint32_t srcIp, uint16_t srcPort) {
	if (!s || !skb) return false;
	if (s->rxCount >= Socket::RXQ || s->rxBytes + skb->len > s->rcvbuf)
		return false;                                 // ring/buffer full: caller drops (UDP has no flow ctl)
	Socket::Dgram* d = &s->rxq[s->rxHead];
	d->skb = skb; d->srcIp = srcIp; d->srcPort = srcPort;
	s->rxHead = (s->rxHead + 1) % Socket::RXQ;
	s->rxCount++;
	s->rxBytes += skb->len;
	if (g_wake) g_wake(&s->rxWait);
	return true;
}

Socket* socketLookupUdp(uint32_t dstIp, uint16_t dstPort, uint32_t srcIp, uint16_t srcPort) {
	Socket* best = 0;
	for (int i = 0; i < SOCK_N; i++) {
		Socket* s = &g_socks[i];
		if (!s->used || s->type != SOCK_DGRAM || !s->bound) continue;
		if (s->localPort != dstPort) continue;
		if (s->localIp != 0 && s->localIp != dstIp) continue;
		// Prefer a connected socket whose peer matches (more specific than a wildcard listener).
		if (s->connected) {
			if (s->remoteIp == srcIp && s->remotePort == srcPort) return s;
			continue;
		}
		if (!best) best = s;
	}
	return best;
}

void socketForEachRaw(int protocol, void (*fn)(Socket*, void*), void* ctx) {
	for (int i = 0; i < SOCK_N; i++) {
		Socket* s = &g_socks[i];
		if (s->used && s->type == SOCK_RAW && (s->protocol == 0 || s->protocol == protocol))
			fn(s, ctx);
	}
}

int socketSlots() { return SOCK_N; }
Socket* socketAt(int i) {
	if (i < 0 || i >= SOCK_N) return 0;
	return g_socks[i].used ? &g_socks[i] : 0;
}

void socketReset() {
	for (int i = 0; i < SOCK_N; i++) {
		Socket* s = &g_socks[i];
		while (s->used && s->rxCount > 0) { netbufFree(s->rxq[s->rxTail].skb); s->rxTail = (s->rxTail+1)%Socket::RXQ; s->rxCount--; }
		s->used = false;
	}
	g_wake = 0; g_nextEphemeral = 32768;
}

}  // namespace kernel
