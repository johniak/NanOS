/*
 * Unix.cpp — AF_UNIX core (see Unix.h). Pure MI: no VFS/scheduler. STREAM is a refcounted
 * bidirectional byte channel (two per-direction rings) wired to two Socket endpoints; DGRAM reuses
 * the Socket RXQ ring + socketDeliver so message boundaries fall out for free. Naming is resolved
 * by scanning net/Socket.cpp's g_socks table (socketAt) — the same authority /proc/net/unix reads.
 */
#include "Unix.h"
#include "Socket.h"
#include "NetBuf.h"
#include <string.h>

namespace kernel {

namespace {

// One bidirectional channel shared by a connected STREAM pair. Endpoint with dir d writes buf[d]
// and reads buf[1-d]; its peer is end[1-d]. refs starts at 2 and a buffer survives until BOTH ends
// close, so a peer can still drain bytes after the other end has gone.
struct UnixChannel {
	static const int CAP = 16 * 1024;     // per-direction byte ring
	unsigned char buf[2][CAP];
	int  head[2], tail[2], count[2];
	Socket* end[2];                       // the two endpoints (0 once that end closes)
	bool shut[2];                         // writer end[d] has shut/closed its write side
	int  refs;
};

struct UnixState {
	char path[UNIX_PATH_MAX];
	unsigned pathLen;                     // 0 = unbound
	bool listening;
	static const int BACKLOG = 16;
	Socket* backlog[BACKLOG];             // server-side sockets awaiting accept
	int blHead, blTail, blCount, blMax;
	UnixChannel* chan; int dir;           // connected STREAM channel (0 if not connected)
	Socket* dgramPeer;                    // connect()ed default DGRAM destination (0 if none)
};

UnixState* st(Socket* s)             { return s ? (UnixState*) s->un : 0; }
const UnixState* st(const Socket* s) { return s ? (const UnixState*) s->un : 0; }

int ringWrite(UnixChannel* c, int d, const unsigned char* p, int n) {
	int w = 0;
	while (w < n && c->count[d] < UnixChannel::CAP) {
		c->buf[d][c->head[d]] = p[w++];
		c->head[d] = (c->head[d] + 1) % UnixChannel::CAP;
		c->count[d]++;
	}
	return w;
}
int ringRead(UnixChannel* c, int d, unsigned char* p, int n, bool peek) {
	int r = 0, tail = c->tail[d];
	while (r < n && r < c->count[d]) {
		p[r++] = c->buf[d][tail];
		tail = (tail + 1) % UnixChannel::CAP;
	}
	if (!peek) { c->tail[d] = tail; c->count[d] -= r; }
	return r;
}

bool bytesEq(const char* a, const char* b, unsigned n) {   // memcmp isn't in the freestanding libc
	for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return false;
	return true;
}

// Find an AF_UNIX socket bound to `path` (optionally required to be listening). Skips `self`.
Socket* findBound(const char* path, unsigned pathLen, int type, bool listening, Socket* self) {
	for (int i = 0; i < socketSlots(); i++) {
		Socket* s = socketAt(i);
		if (!s || s == self || s->domain != AF_UNIX || s->type != type) continue;
		UnixState* u = st(s);
		if (!u || u->pathLen != pathLen || pathLen == 0) continue;
		if (!bytesEq(u->path, path, pathLen)) continue;
		if (listening && !u->listening) continue;
		return s;
	}
	return 0;
}

}  // namespace

int unixAttach(Socket* s) {
	UnixState* u = new UnixState;
	if (!u) return -SOCK_ENOBUFS;
	memset(u, 0, sizeof *u);
	u->blMax = UnixState::BACKLOG;
	s->un = u;
	return 0;
}

void unixDetach(Socket* s) {
	UnixState* u = st(s);
	if (!u) return;
	// A listener still holding un-accepted connections: close each (frees their channel ref too).
	while (u->blCount > 0) {
		Socket* sv = u->backlog[u->blTail];
		u->backlog[u->blTail] = 0;
		u->blTail = (u->blTail + 1) % UnixState::BACKLOG;
		u->blCount--;
		socketClose(sv);
	}
	if (u->chan) {
		UnixChannel* c = u->chan;
		int d = u->dir;
		c->shut[d] = true;          // our write side ends -> peer reads EOF after draining
		c->end[d] = 0;              // we're gone; peer must not dereference us
		if (c->end[1 - d]) socketWakeReaders(c->end[1 - d]);
		if (--c->refs == 0) delete c;
		u->chan = 0;
	}
	delete u;
	s->un = 0;
}

int unixBind(Socket* s, const char* path, unsigned pathLen) {
	UnixState* u = st(s);
	if (!u || !path || pathLen == 0 || pathLen > UNIX_PATH_MAX) return -SOCK_EINVAL;
	if (u->pathLen) return -SOCK_EINVAL;                       // already bound
	if (findBound(path, pathLen, s->type, false, s)) return -SOCK_EADDRINUSE;
	memcpy(u->path, path, pathLen);
	u->pathLen = pathLen;
	s->bound = true;
	return 0;
}

int unixListen(Socket* s, int backlog) {
	UnixState* u = st(s);
	if (!u) return -SOCK_EINVAL;
	if (s->type != SOCK_STREAM) return -SOCK_EOPNOTSUPP;
	if (!u->pathLen) return -SOCK_EINVAL;                      // must bind first
	u->listening = true;
	u->blMax = backlog < 1 ? 1 : (backlog > UnixState::BACKLOG ? UnixState::BACKLOG : backlog);
	return 0;
}

int unixConnect(Socket* s, const char* path, unsigned pathLen) {
	UnixState* u = st(s);
	if (!u || !path || pathLen == 0) return -SOCK_EINVAL;
	if (s->connected) return -SOCK_EISCONN;

	if (s->type == SOCK_DGRAM) {                               // record default destination only
		Socket* peer = findBound(path, pathLen, SOCK_DGRAM, false, s);
		if (!peer) return -SOCK_ECONNREFUSED;
		u->dgramPeer = peer;
		s->connected = true;
		return 0;
	}

	// STREAM: pair with a listening socket via a fresh channel; queue the server end for accept.
	Socket* l = findBound(path, pathLen, SOCK_STREAM, true, s);
	if (!l) return -SOCK_ECONNREFUSED;
	UnixState* lu = st(l);
	if (lu->blCount >= lu->blMax) return -SOCK_EAGAIN;        // backlog full: caller may retry/block

	Socket* server = socketCreateRaw(AF_UNIX, SOCK_STREAM, 0);
	if (!server) return -SOCK_ENOBUFS;
	UnixChannel* c = new UnixChannel;
	if (!c) { socketClose(server); return -SOCK_ENOBUFS; }
	memset(c, 0, sizeof *c);
	c->refs = 2;
	c->end[0] = s;      st(s)->chan = c;      st(s)->dir = 0;   // client = dir 0
	c->end[1] = server; st(server)->chan = c; st(server)->dir = 1;   // server = dir 1
	server->connected = true;
	s->connected = true;

	lu->backlog[lu->blHead] = server;
	lu->blHead = (lu->blHead + 1) % UnixState::BACKLOG;
	lu->blCount++;
	socketWakeReaders(l);                                     // wake accept()
	return 0;
}

Socket* unixAccept(Socket* s, int* err) {
	UnixState* u = st(s);
	if (!u || !u->listening) { if (err) *err = -SOCK_EINVAL; return 0; }
	if (u->blCount == 0) { if (err) *err = -SOCK_EAGAIN; return 0; }
	Socket* sv = u->backlog[u->blTail];
	u->backlog[u->blTail] = 0;
	u->blTail = (u->blTail + 1) % UnixState::BACKLOG;
	u->blCount--;
	if (err) *err = 0;
	return sv;
}

int unixSend(Socket* s, const void* buf, unsigned len, const char* path, unsigned pathLen) {
	UnixState* u = st(s);
	if (!u) return -SOCK_EINVAL;

	if (s->type == SOCK_STREAM) {
		UnixChannel* c = u->chan;
		if (!c) return -SOCK_ENOTCONN;
		if (c->shut[u->dir] || c->end[1 - u->dir] == 0) return -SOCK_EPIPE;   // peer gone
		int w = ringWrite(c, u->dir, (const unsigned char*) buf, (int) len);
		if (w == 0 && len > 0) return -SOCK_EAGAIN;          // buffer full: dispatch blocks
		if (c->end[1 - u->dir]) socketWakeReaders(c->end[1 - u->dir]);
		return w;
	}

	// DGRAM: deliver one message to the destination's receive ring.
	Socket* peer = 0;
	if (path && pathLen) peer = findBound(path, pathLen, SOCK_DGRAM, false, s);
	else if (s->connected && u->dgramPeer && u->dgramPeer->used) peer = u->dgramPeer;
	else return -SOCK_EDESTADDRREQ;
	if (!peer) return -SOCK_ECONNREFUSED;
	NetBuf* skb = netbufAlloc();
	if (!skb) return -SOCK_ENOBUFS;
	skb->reserve(0);
	memcpy(skb->put((int) len), buf, len);
	if (!socketDeliver(peer, skb, 0, 0)) { netbufFree(skb); return -SOCK_EAGAIN; }
	return (int) len;
}

int unixRecv(Socket* s, void* buf, unsigned len, int flags, char* fromPath, unsigned* fromLen) {
	UnixState* u = st(s);
	if (!u) return -SOCK_EINVAL;
	if (fromLen) *fromLen = 0;                                // sender name reported only for STREAM-less? none yet

	if (s->type == SOCK_STREAM) {
		UnixChannel* c = u->chan;
		if (!c) return -SOCK_ENOTCONN;
		int rd = 1 - u->dir;                                  // we read what the peer wrote
		if (c->count[rd] == 0) {
			if (c->shut[rd] || c->end[rd] == 0) return 0;     // peer closed + drained -> EOF
			return -SOCK_EAGAIN;
		}
		int n = ringRead(c, rd, (unsigned char*) buf, (int) len, (flags & MSG_PEEK) != 0);
		if (!(flags & MSG_PEEK) && c->end[rd]) socketWakeReaders(c->end[rd]);   // wake blocked writer
		return n;
	}

	// DGRAM: pull one message from our own ring (delivered by a sender via socketDeliver).
	if (s->rxCount == 0) return -SOCK_EAGAIN;
	Socket::Dgram* d = &s->rxq[s->rxTail];
	int avail = d->skb->len;
	int n = (int) len < avail ? (int) len : avail;
	memcpy(buf, d->skb->head(), n);
	if (!(flags & MSG_PEEK)) {
		s->rxBytes -= d->skb->len;
		netbufFree(d->skb);
		s->rxTail = (s->rxTail + 1) % Socket::RXQ;
		s->rxCount--;
	}
	return n;
}

bool unixReadable(const Socket* s) {
	const UnixState* u = st(s);
	if (!u) return false;
	if (u->listening) return u->blCount > 0;
	if (s->type == SOCK_STREAM) {
		const UnixChannel* c = u->chan;
		if (!c) return false;
		int rd = 1 - u->dir;
		return c->count[rd] > 0 || c->shut[rd] || c->end[rd] == 0;   // data or EOF both wake a reader
	}
	return s->rxCount > 0;
}

bool unixWritable(const Socket* s) {
	const UnixState* u = st(s);
	if (!u) return false;
	if (s->type == SOCK_STREAM) {
		const UnixChannel* c = u->chan;
		if (!c) return false;
		if (c->shut[u->dir] || c->end[1 - u->dir] == 0) return true;   // EPIPE is "ready" (write returns error)
		return c->count[u->dir] < UnixChannel::CAP;
	}
	return true;                                              // DGRAM: always writable
}

int unixSocketpair(int type, int /*protocol*/, Socket** a, Socket** b) {
	if (type != SOCK_STREAM) return -SOCK_EOPNOTSUPP;         // SEQPACKET/DGRAM pairs: future
	Socket* s0 = socketCreateRaw(AF_UNIX, SOCK_STREAM, 0);
	Socket* s1 = socketCreateRaw(AF_UNIX, SOCK_STREAM, 0);
	if (!s0 || !s1) { if (s0) socketClose(s0); if (s1) socketClose(s1); return -SOCK_ENOBUFS; }
	UnixChannel* c = new UnixChannel;
	if (!c) { socketClose(s0); socketClose(s1); return -SOCK_ENOBUFS; }
	memset(c, 0, sizeof *c);
	c->refs = 2;
	c->end[0] = s0; st(s0)->chan = c; st(s0)->dir = 0; s0->connected = true;
	c->end[1] = s1; st(s1)->chan = c; st(s1)->dir = 1; s1->connected = true;
	*a = s0; *b = s1;
	return 0;
}

unsigned unixGetName(const Socket* s, char* out, unsigned cap) {
	const UnixState* u = st(s);
	if (!u || u->pathLen == 0) return 0;
	unsigned n = u->pathLen < cap ? u->pathLen : cap;
	if (out) memcpy(out, u->path, n);
	return u->pathLen;
}

bool unixIsListening(const Socket* s) { const UnixState* u = st(s); return u && u->listening; }

const Socket* unixPeerOf(const Socket* s) {
	const UnixState* u = st(s);
	if (!u || !u->chan) return 0;
	return u->chan->end[1 - u->dir];
}

void unixReset() { /* sockets (and their UnixState via socketClose) are freed by socketReset() */ }

}  // namespace kernel
