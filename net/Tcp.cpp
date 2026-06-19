#include "Tcp.h"
#include "Socket.h"
#include "Ip.h"
#include "Ether.h"     // NET_HEADROOM
#include "Route.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Net.h"
#include "WaitQueue.h"
#include "NetStats.h"   // /proc/net/snmp counters
#include <string.h>

namespace kernel {

namespace {

const int SNDBUF = 8192;       // restored from the FAZA 9 trim (4096) — see the user-window move
const int RCVBUF = 8192;
const int MSS_MAX = 1460;
const int OOO_N = 4;            // out-of-order pending segments
const int TCB_N = 64;          // concurrent TCP connections. Raised 16->64: a browser (NetSurf/curl)
                               // keeps connections ALIVE in a pool, and tcbAlloc only reclaims
                               // TIME_WAIT (never idle ESTABLISHED) — so a page's keep-alive pool
                               // filled all 16 TCBs and the NEXT connection failed ("host doesn't
                               // work" after browsing). Each TCB is ~16.5KB (8K snd+8K rcv); 64 =>
                               // ~1MB BSS, trivial at 512MB RAM. (Was 4 in the 0x800000 era.)
const int ACCEPT_N = 8;        // pending accept queue per listener

// Timer constants (ticks == ms; the net-timer thread calls tcpTick).
const int RTO_INIT = 1000, RTO_MIN = 200, RTO_MAX = 60000;
const int SYN_RETRIES_MAX = 5;   // give up a half-open handshake after this many RTO firings (Linux ~6)
// Our advertised window-scale shift. RCVBUF (8 KiB) fits unscaled in the 16-bit window field, so we
// advertise shift 0 (no quantization of our own window) — but we ALWAYS send the option so scaling is
// negotiated and we honour a peer that advertises a large scaled window (RFC 7323 §2).
const int RCV_WSCALE = 0;
const int DELAY_ACK = 40;        // delayed-ACK timeout (ms), flushed by the 50 ms tcpTick (Linux: ≤40 ms)
const int PERSIST_INIT = 5000, PERSIST_MAX = 60000;   // zero-window probe backoff bounds (ms)
const int MSL = 30000;         // 2*MSL = 60 s TIME-WAIT

struct Ooo { uint32_t seq; int len; bool used; unsigned char data[MSS_MAX]; };

struct Tcb {
	bool used;
	Socket* sock;              // nullable (orphaned after close)
	int state;
	uint32_t localIp, remoteIp;
	uint16_t localPort, remotePort;

	uint32_t iss, snd_una, snd_nxt, snd_wnd;
	uint32_t irs, rcv_nxt;
	uint16_t mss;
	uint8_t  sndWscale;        // peer's window-scale shift, applied to its advertised window (RFC 7323)
	bool     wscaleOk;         // window scaling negotiated (both SYNs carried the option)
	bool     tsOk;             // timestamps negotiated (RFC 7323 §3) — both SYNs carried the option
	uint32_t tsRecent;         // most recent in-window peer TSval, echoed back in our TSecr
	bool     sackOk;           // selective ACK negotiated (RFC 2018) — both SYNs carried SACK-permitted
	struct { uint32_t l, r; } sacked[4]; int sackedN;   // sender scoreboard: peer-SACKed ranges
	bool     ackPending;       // a delayed ACK is owed (flush on the next tick if not piggy-backed)
	unsigned ackDeadline; int ackSegs;   // delayed-ACK deadline + in-order segments since the last ACK
	unsigned persistDeadline; int persistBackoff;   // zero-window probe timer (RFC 1122 §4.2.2.17)
	bool     keepalive;        // SO_KEEPALIVE
	bool     nodelay;          // TCP_NODELAY: disable Nagle (send small segments immediately)
	unsigned keepIdle, keepIntvl; int keepCnt;      // TCP_KEEPIDLE/INTVL/CNT (ms, ms, count)
	unsigned keepDeadline; int keepProbes;          // next keepalive event + unanswered probe count

	uint32_t cwnd, ssthresh;
	int dupacks;
	int rtxCount;              // consecutive RTO firings (handshake give-up + backoff bookkeeping)

	int srtt, rttvar, rto;
	uint32_t rttSeq; unsigned rttStart; bool rttPending;

	unsigned rtoDeadline, timeWaitDeadline;
	bool finSent; uint32_t finSeq;
	bool peerFin;

	unsigned char sndBuf[SNDBUF]; int sndLen;          // buffered bytes starting at snd_una
	unsigned char rcvBuf[RCVBUF]; int rcvHead, rcvTail, rcvCount;
	Ooo ooo[OOO_N];

	bool isListen;
	Tcb* acceptq[ACCEPT_N]; int acceptCount;           // completed connections waiting for accept()
	Tcb* parent;                                       // listener that birthed us (for accept queue)
};

Tcb g_tcbs[TCB_N];
unsigned (*g_clock)() = 0;
TcpNewSockFn g_newSock = 0;
uint32_t g_isnCounter = 0x10000;   // monotonic ISN base (deterministic for tests; varied per conn)

unsigned now() { return g_clock ? g_clock() : 0; }

// wrap-safe sequence comparisons
inline bool seqLt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) < 0; }
inline bool seqLeq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
inline bool seqGt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) > 0; }
inline bool seqGeq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static void tcbInit(Tcb* t) {
	memset(t, 0, sizeof(*t));
	t->used = true; t->state = TCP_CLOSED;
	t->mss = 536;                  // default until the peer's MSS option is seen
	t->cwnd = 1 * 1460; t->ssthresh = 65535;
	t->srtt = 0; t->rttvar = 0; t->rto = RTO_INIT;
	t->snd_wnd = 4096;
	t->keepIdle = 7200000; t->keepIntvl = 75000; t->keepCnt = 9;   // Linux defaults (2h / 75s / 9)
	t->nodelay = false;
}
Tcb* tcbAlloc() {
	for (int i = 0; i < TCB_N; i++)
		if (!g_tcbs[i].used) { tcbInit(&g_tcbs[i]); return &g_tcbs[i]; }
	// Table full: reclaim the oldest TIME_WAIT slot. A TIME_WAIT TCB is only idling out 2*MSL to
	// absorb stray retransmits; recycling one under allocation pressure is safe and is what Linux
	// does (tcp_tw_reuse), so a burst of short connections to a server that actively closes (e.g.
	// HTTP/1.0 Connection: close) does not exhaust the small TCB table and stall new accepts.
	Tcb* victim = 0;
	for (int i = 0; i < TCB_N; i++)
		if (g_tcbs[i].used && g_tcbs[i].state == TCP_TIME_WAIT)
			if (!victim || (int) (g_tcbs[i].timeWaitDeadline - victim->timeWaitDeadline) < 0)
				victim = &g_tcbs[i];
	if (victim) { tcbInit(victim); return victim; }
	return 0;
}
void tcbFree(Tcb* t) { if (t) t->used = false; }

int rcvFree(Tcb* t) { return RCVBUF - t->rcvCount; }

// --- SACK (RFC 2018) ---------------------------------------------------------------------------

// Receiver: derive SACK blocks (left/right edges) from the out-of-order buffer, merging adjacent or
// overlapping ranges. Returns the number of blocks (capped at 3 so the option fits beside timestamps).
int buildSackBlocks(Tcb* t, uint32_t* out) {
	uint32_t l[OOO_N], r[OOO_N]; int n = 0;
	for (int i = 0; i < OOO_N; i++)
		if (t->ooo[i].used) { l[n] = t->ooo[i].seq; r[n] = t->ooo[i].seq + t->ooo[i].len; n++; }
	bool merged = true;
	while (merged) {
		merged = false;
		for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
			if (seqLeq(l[j], r[i]) && seqLeq(l[i], r[j])) {        // touch or overlap -> merge j into i
				if (seqLt(l[j], l[i])) l[i] = l[j];
				if (seqGt(r[j], r[i])) r[i] = r[j];
				l[j] = l[n - 1]; r[j] = r[n - 1]; n--; merged = true;
			}
	}
	int nb = n < 3 ? n : 3;
	for (int i = 0; i < nb; i++) { out[i * 2] = l[i]; out[i * 2 + 1] = r[i]; }
	return nb;
}

// Sender: replace the scoreboard with the peer's reported SACK blocks, clipped to the still-unacked
// window (snd_una, snd_nxt). The peer re-reports its full SACK state in each ACK, so replacing is correct.
void sackRecord(Tcb* t, const uint32_t* blk, int n) {
	t->sackedN = 0;
	for (int i = 0; i < n && t->sackedN < 4; i++) {
		uint32_t l = blk[i * 2], r = blk[i * 2 + 1];
		if (seqLeq(r, t->snd_una) || !seqLt(l, t->snd_nxt)) continue;   // wholly acked or beyond what we sent
		if (seqLt(l, t->snd_una)) l = t->snd_una;
		if (seqGt(r, t->snd_nxt)) r = t->snd_nxt;
		if (seqLt(l, r)) { t->sacked[t->sackedN].l = l; t->sacked[t->sackedN].r = r; t->sackedN++; }
	}
}

// Sender: pick the next chunk to (re)transmit from snd_una, skipping ranges the peer already SACKed
// and stopping before the next SACKed edge. Writes the seq into *seqOut; returns the chunk length
// (0 = nothing left to retransmit). With an empty scoreboard this is just min(mss, sndLen) at snd_una.
int retxChunk(Tcb* t, uint32_t* seqOut) {
	uint32_t s = t->snd_una;
	bool moved = true;
	while (moved) {                                        // skip past contiguous SACKed ranges
		moved = false;
		for (int i = 0; i < t->sackedN; i++)
			if (seqLeq(t->sacked[i].l, s) && seqLt(s, t->sacked[i].r)) { s = t->sacked[i].r; moved = true; }
	}
	int off = (int) (s - t->snd_una);
	if (off >= t->sndLen) return 0;                        // everything up to snd_nxt is SACKed
	int maxLen = t->sndLen - off;
	if (maxLen > t->mss) maxLen = t->mss;
	for (int i = 0; i < t->sackedN; i++)                  // don't overrun into the next SACKed block
		if (seqGt(t->sacked[i].l, s)) { int d = (int) (t->sacked[i].l - s); if (d < maxLen) maxLen = d; }
	*seqOut = s;
	return maxLen;
}

// Build + send one TCP segment: flags, `dataOff` bytes of payload from the send buffer (len),
// optional MSS option on SYN. Updates checksum. Does NOT advance snd_nxt (caller decides).
void sendSeg(Tcb* t, uint8_t flags, uint32_t seq, const unsigned char* data, int len) {
	NetBuf* skb = netbufAlloc();
	if (!skb) return;
	g_netStats.tcpOutSegs++;
	if (flags & TCP_RST) g_netStats.tcpOutRsts++;
	skb->reserve(NET_HEADROOM);
	// Build TCP options. SYN carries [MSS, (sackOK slot), TS, NOP, wscale]; once timestamps are
	// negotiated every segment carries [NOP, NOP, TS]. Timestamps are offered on the initial SYN
	// (active open) and mirrored on the SYN-ACK / data only if the peer offered them (tsOk).
	bool isSyn    = (flags & TCP_SYN) != 0;
	bool wantTs   = t->tsOk   || (isSyn && !(flags & TCP_ACK));
	bool wantSack = t->sackOk || (isSyn && !(flags & TCP_ACK));   // offer SACK-permitted on the initial SYN
	unsigned char opt[40]; int ol = 0;
	if (isSyn) {
		opt[ol++] = 2; opt[ol++] = 4; wr16be(opt + ol, MSS_MAX); ol += 2;     // MSS
		if (wantSack) { opt[ol++] = 4; opt[ol++] = 2; }                       // SACK-permitted
		else { opt[ol++] = 1; opt[ol++] = 1; }                               // (NOPs if not offering)
		if (wantTs) { opt[ol++] = 8; opt[ol++] = 10; wr32be(opt + ol, now()); ol += 4;
			wr32be(opt + ol, t->tsRecent); ol += 4; }                        // timestamp
		opt[ol++] = 1;                                                        // NOP aligning wscale
		opt[ol++] = 3; opt[ol++] = 3; opt[ol++] = (unsigned char) RCV_WSCALE; // window scale
	} else {
		if (wantTs) {
			opt[ol++] = 1; opt[ol++] = 1;                                    // 2 NOPs align the TS option
			opt[ol++] = 8; opt[ol++] = 10; wr32be(opt + ol, now()); ol += 4;
			wr32be(opt + ol, t->tsRecent); ol += 4;
		}
		// Receiver SACK: report our out-of-order ranges so the peer retransmits only the holes.
		if (t->sackOk) {
			uint32_t blk[6]; int nb = buildSackBlocks(t, blk);
			if (nb > 0 && ol + 2 + 2 + 8 * nb <= 40) {
				opt[ol++] = 1; opt[ol++] = 1;                                // 2 NOPs align the SACK option
				opt[ol++] = 5; opt[ol++] = (unsigned char) (2 + 8 * nb);     // SACK kind + length
				for (int i = 0; i < nb; i++) { wr32be(opt + ol, blk[i * 2]); ol += 4; wr32be(opt + ol, blk[i * 2 + 1]); ol += 4; }
			}
		}
	}
	while (ol & 3) opt[ol++] = 1;                                             // pad to a 4-byte boundary
	int optLen = ol;
	if (len) memcpy(skb->put(len), data, len);
	unsigned char* h = skb->push(20 + optLen);
	wr16be(h + 0, t->localPort);
	wr16be(h + 2, t->remotePort);
	wr32be(h + 4, seq);
	wr32be(h + 8, (flags & TCP_ACK) ? t->rcv_nxt : 0);
	h[12] = (unsigned char) (((20 + optLen) / 4) << 4);   // data offset
	h[13] = flags;
	wr16be(h + 14, (uint16_t) rcvFree(t));                // advertised receive window (shift 0)
	wr16be(h + 16, 0);                                    // checksum
	wr16be(h + 18, 0);                                    // urgent ptr
	if (optLen) memcpy(h + 20, opt, optLen);
	uint16_t c = inetPseudoChecksum(t->localIp, t->remoteIp, IPPROTO_TCP, h, 20 + optLen + len);
	wr16be(h + 16, c);
	ipOutput(t->remoteIp, IPPROTO_TCP, skb);
	// Any segment we send carries the current rcv_nxt as its ACK, so it satisfies a delayed ACK.
	if (flags & TCP_ACK) { t->ackPending = false; t->ackSegs = 0; }
}

void armRto(Tcb* t) { t->rtoDeadline = now() + t->rto; }

// Transmit as much new data as the window allows (Reno: min(snd_wnd, cwnd)).
void sendData(Tcb* t) {
	uint32_t win = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
	uint32_t inflight = t->snd_nxt - t->snd_una;
	int sentOff = (int) (t->snd_nxt - t->snd_una);       // offset into sndBuf of next unsent byte
	bool sentAny = false;
	while (sentOff < t->sndLen && inflight < win) {
		int chunk = t->sndLen - sentOff;
		if (chunk > t->mss) chunk = t->mss;
		if ((uint32_t) chunk > win - inflight) chunk = (int) (win - inflight);
		if (chunk <= 0) break;
		// Nagle: hold a small (<MSS) segment while data is already in flight (unless nothing's out).
		// TCP_NODELAY (darkhttpd, interactive servers) disables this — send immediately.
		if (!t->nodelay && chunk < t->mss && inflight > 0 && (sentOff + chunk) >= t->sndLen) break;
		uint8_t fl = TCP_ACK | TCP_PSH;
		sendSeg(t, fl, t->snd_nxt, t->sndBuf + sentOff, chunk);
		t->snd_nxt += chunk;
		inflight += chunk;
		sentOff += chunk;
		sentAny = true;
		if (!t->rttPending) { t->rttPending = true; t->rttSeq = t->snd_nxt; t->rttStart = now(); }
	}
	if (sentAny) armRto(t);
	// Persist timer (RFC 1122 §4.2.2.17): if data is waiting but the peer's window is shut, arm a
	// probe; once the window reopens, cancel it (the loop above resumed sending).
	if (win > 0) { t->persistDeadline = 0; t->persistBackoff = PERSIST_INIT; }
	else if (sentOff < t->sndLen && !t->persistDeadline) {
		t->persistBackoff = PERSIST_INIT;
		t->persistDeadline = now() + PERSIST_INIT;
	}
}

// Deliver in-order payload into the recv buffer; returns bytes accepted.
int rcvAppend(Tcb* t, const unsigned char* data, int len) {
	int free = rcvFree(t);
	if (len > free) len = free;
	for (int i = 0; i < len; i++) {
		t->rcvBuf[t->rcvHead] = data[i];
		t->rcvHead = (t->rcvHead + 1) % RCVBUF;
		t->rcvCount++;
	}
	return len;
}

void drainOoo(Tcb* t) {
	bool progress = true;
	while (progress) {
		progress = false;
		for (int i = 0; i < OOO_N; i++) {
			Ooo* o = &t->ooo[i];
			if (!o->used) continue;
			if (seqLeq(o->seq, t->rcv_nxt) && seqGt(o->seq + o->len, t->rcv_nxt)) {
				int skip = (int) (t->rcv_nxt - o->seq);
				int got = rcvAppend(t, o->data + skip, o->len - skip);
				t->rcv_nxt += got;
				o->used = false;
				progress = true;
			} else if (seqLeq(o->seq + o->len, t->rcv_nxt)) {
				o->used = false;   // fully stale
			}
		}
	}
}

void queueOoo(Tcb* t, uint32_t seq, const unsigned char* data, int len) {
	if (len > MSS_MAX) len = MSS_MAX;
	for (int i = 0; i < OOO_N; i++)
		if (!t->ooo[i].used) {
			t->ooo[i].used = true; t->ooo[i].seq = seq; t->ooo[i].len = len;
			memcpy(t->ooo[i].data, data, len);
			return;
		}
	// queue full: drop (sender will retransmit)
}

void enterTimeWait(Tcb* t) { t->state = TCP_TIME_WAIT; t->timeWaitDeadline = now() + 2 * MSL; }

void sockWake(Tcb* t) { if (t->sock) socketWakeReaders(t->sock); }

Tcb* lookup(uint32_t la, uint16_t lp, uint32_t ra, uint16_t rp) {
	Tcb* listener = 0;
	for (int i = 0; i < TCB_N; i++) {
		Tcb* t = &g_tcbs[i];
		if (!t->used) continue;
		if (t->localPort != lp) continue;
		if (t->isListen) { if (t->localIp == 0 || t->localIp == la) listener = t; continue; }
		// A TCP_CLOSED tcb is a dead connection still pinned by an un-closed socket (e.g. after a
		// peer RST). It must NOT match: when the peer reuses that ephemeral 4-tuple for a fresh
		// connection, the SYN has to reach the LISTEN socket — otherwise the zombie shadows the
		// listener, no SYN-ACK is sent, and the port wedges permanently. (This is the burst-churn
		// "TCP stops accepting after N connections" bug: a reused source port collides with a
		// not-yet-freed CLOSED tcb.) Skipping it lets the SYN fall through to the listener below.
		if (t->state == TCP_CLOSED) continue;
		if (t->remotePort == rp && t->remoteIp == ra && (t->localIp == 0 || t->localIp == la))
			return t;
	}
	return listener;   // fall back to a LISTEN socket for a new SYN
}

}  // namespace anon

// ---- RTT / congestion helpers ----
namespace {
void rttUpdate(Tcb* t, int measured) {
	if (measured < 1) measured = 1;
	if (t->srtt == 0) { t->srtt = measured; t->rttvar = measured / 2; }
	else {
		int err = measured - t->srtt;
		t->srtt += err / 8;
		int aerr = err < 0 ? -err : err;
		t->rttvar += (aerr - t->rttvar) / 4;
	}
	t->rto = t->srtt + 4 * t->rttvar;
	if (t->rto < RTO_MIN) t->rto = RTO_MIN;
	if (t->rto > RTO_MAX) t->rto = RTO_MAX;
}
void onAckCwnd(Tcb* t, int acked) {
	if (acked <= 0) return;
	if (t->cwnd < t->ssthresh) t->cwnd += t->mss;                       // slow start
	else t->cwnd += (uint32_t) t->mss * t->mss / (t->cwnd ? t->cwnd : 1); // congestion avoidance
}
}  // namespace

void tcpSetClock(unsigned (*fn)()) { g_clock = fn; }
void tcpSetNewSockHook(TcpNewSockFn fn) { g_newSock = fn; }

void tcpInit() { ipSetHandler(IPPROTO_TCP, tcpRx); }

// ---- the segment handler ----
void tcpRx(NetBuf* skb) {
	if (!skb) { return; }
	if (skb->len < 20) { g_netStats.tcpInErrs++; netbufFree(skb); return; }
	const unsigned char* h = skb->head();
	if (inetPseudoChecksum(skb->saddr, skb->daddr, IPPROTO_TCP, h, skb->len) != 0) { g_netStats.tcpInErrs++; netbufFree(skb); return; }
	g_netStats.tcpInSegs++;
	uint16_t sport = rd16be(h + 0), dport = rd16be(h + 2);
	uint32_t seq = rd32be(h + 4), ack = rd32be(h + 8);
	int doff = (h[12] >> 4) * 4;
	uint8_t flags = h[13];
	uint16_t wnd = rd16be(h + 14);
	if (doff < 20 || doff > skb->len) { netbufFree(skb); return; }
	const unsigned char* payload = h + doff;
	int plen = skb->len - doff;

	// peer options: MSS (2), window scale (3), SACK-permitted (4), SACK blocks (5), timestamps (8).
	uint16_t peerMss = 0;
	uint8_t peerWscale = 0; bool peerHasWscale = false;
	uint32_t peerTsVal = 0, peerTsEcr = 0; bool peerHasTs = false;
	bool peerSackOk = false;
	uint32_t rxSack[8]; int rxSackN = 0;
	for (int i = 20; i + 1 < doff; ) {
		uint8_t kind = h[i];
		if (kind == 0) break;
		if (kind == 1) { i++; continue; }
		uint8_t olen = h[i + 1];
		if (olen < 2 || i + olen > doff) break;
		if (kind == 2 && olen == 4) peerMss = rd16be(h + i + 2);
		else if (kind == 3 && olen == 3) { peerWscale = h[i + 2]; peerHasWscale = true;
			if (peerWscale > 14) peerWscale = 14; }   // RFC 7323: cap shift at 14
		else if (kind == 4 && olen == 2) peerSackOk = true;
		else if (kind == 5 && olen >= 10 && (olen - 2) % 8 == 0) {
			int nb = (olen - 2) / 8;
			for (int b = 0; b < nb && rxSackN < 4; b++) {
				rxSack[rxSackN * 2]     = rd32be(h + i + 2 + b * 8);
				rxSack[rxSackN * 2 + 1] = rd32be(h + i + 2 + b * 8 + 4);
				rxSackN++;
			}
		}
		else if (kind == 8 && olen == 10) { peerTsVal = rd32be(h + i + 2); peerTsEcr = rd32be(h + i + 6); peerHasTs = true; }
		i += olen;
	}

	Tcb* t = lookup(skb->daddr, dport, skb->saddr, sport);
	if (!t) {
		// No socket: RST (unless this is itself a RST). Send a courtesy reset to the peer.
		if (!(flags & TCP_RST)) {
			// Build a minimal TCB-less RST.
			Tcb tmp; memset(&tmp, 0, sizeof(tmp));
			tmp.localIp = skb->daddr; tmp.remoteIp = skb->saddr;
			tmp.localPort = dport; tmp.remotePort = sport;
			if (flags & TCP_ACK) sendSeg(&tmp, TCP_RST, ack, 0, 0);
			else { tmp.rcv_nxt = seq + plen + ((flags & TCP_SYN) ? 1 : 0); sendSeg(&tmp, TCP_RST | TCP_ACK, 0, 0, 0); }
		}
		netbufFree(skb);
		return;
	}

	if (flags & TCP_RST) {
		if (t->state != TCP_LISTEN) { if (t->sock) t->sock->soError = SOCK_ECONNREFUSED; t->state = TCP_CLOSED; sockWake(t); }
		netbufFree(skb);
		return;
	}

	// PAWS (RFC 7323 §5): in a synchronized connection a non-SYN segment whose timestamp predates
	// ts_recent is a stale duplicate from an earlier incarnation — drop it (modular TS comparison).
	if (t->tsOk && peerHasTs && !(flags & TCP_SYN) &&
	    t->state != TCP_SYN_SENT && t->state != TCP_LISTEN && seqLt(peerTsVal, t->tsRecent)) {
		netbufFree(skb);
		return;
	}
	// ts_recent update (RFC 7323 §4.3): adopt the newest in-window peer timestamp to echo back.
	if (t->tsOk && peerHasTs && seqLeq(seq, t->rcv_nxt) && seqGeq(peerTsVal, t->tsRecent))
		t->tsRecent = peerTsVal;
	// Any segment from the peer is activity — restart the keepalive idle timer.
	if (t->keepalive) { t->keepProbes = 0; t->keepDeadline = now() + t->keepIdle; }

	switch (t->state) {
	case TCP_SYN_SENT: {
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
			if (ack != t->snd_nxt) { netbufFree(skb); return; }      // wrong ACK -> drop (would RST in full impl)
			t->irs = seq; t->rcv_nxt = seq + 1;
			// Window scaling is on only if BOTH SYNs carried the option (we always send it). The
			// window in the SYN-ACK itself is NOT scaled (RFC 7323 §2.2) — scaling starts after.
			t->wscaleOk = peerHasWscale; t->sndWscale = peerHasWscale ? peerWscale : 0;
			t->tsOk = peerHasTs; if (peerHasTs) t->tsRecent = peerTsVal;   // RFC 7323 §3 negotiation
			t->sackOk = peerSackOk;                                        // RFC 2018 negotiation
			t->snd_una = ack; t->snd_wnd = wnd;
			if (peerMss) t->mss = peerMss < MSS_MAX ? peerMss : MSS_MAX;
			t->state = TCP_ESTABLISHED;
			t->rtoDeadline = 0;
			sendSeg(t, TCP_ACK, t->snd_nxt, 0, 0);
			sockWake(t);
		} else if (flags & TCP_SYN) {                                 // simultaneous open
			t->irs = seq; t->rcv_nxt = seq + 1; t->state = TCP_SYN_RCVD;
			sendSeg(t, TCP_SYN | TCP_ACK, t->iss, 0, 0);
		}
		netbufFree(skb);
		return;
	}
	case TCP_LISTEN: {
		if (flags & TCP_SYN) {
			Tcb* c = tcbAlloc();
			if (!c) { netbufFree(skb); return; }
			c->localIp = skb->daddr; c->localPort = dport;
			c->remoteIp = skb->saddr; c->remotePort = sport;
			c->irs = seq; c->rcv_nxt = seq + 1;
			c->iss = g_isnCounter; g_isnCounter += 0x4000;
			c->snd_una = c->iss; c->snd_nxt = c->iss + 1;
			c->snd_wnd = wnd ? wnd : 4096;
			c->wscaleOk = peerHasWscale; c->sndWscale = peerHasWscale ? peerWscale : 0;   // our SYN-ACK carries ours
			c->tsOk = peerHasTs; if (peerHasTs) c->tsRecent = peerTsVal;
			c->sackOk = peerSackOk;
			if (peerMss) c->mss = peerMss < MSS_MAX ? peerMss : MSS_MAX;
			c->state = TCP_SYN_RCVD; c->parent = t;
			g_netStats.tcpPassiveOpens++;
			sendSeg(c, TCP_SYN | TCP_ACK, c->iss, 0, 0);
			armRto(c);
		}
		netbufFree(skb);
		return;
	}
	default: break;
	}

	// Established-ish states: validate the ACK, accept data, handle FIN.
	if (flags & TCP_ACK) {
		// A zero-window probe byte was sent without advancing snd_nxt; if the peer accepted it, let
		// snd_nxt catch up so the ack falls in range and the buffered byte is treated as delivered.
		if (seqGt(ack, t->snd_nxt) && seqLeq(ack, t->snd_una + (uint32_t) t->sndLen)) t->snd_nxt = ack;
		if (t->sackOk && rxSackN) sackRecord(t, rxSack, rxSackN);   // refresh the sender scoreboard
		if (seqGt(ack, t->snd_una) && seqLeq(ack, t->snd_nxt)) {
			int acked = (int) (ack - t->snd_una);
			// free acked bytes from the send buffer
			int dataAcked = acked;
			if (t->finSent && seqGt(ack, t->finSeq)) dataAcked -= 1;   // FIN consumes one seq
			if (dataAcked > 0) {
				int move = dataAcked <= t->sndLen ? dataAcked : t->sndLen;
				memmove(t->sndBuf, t->sndBuf + move, t->sndLen - move);
				t->sndLen -= move;
			}
			t->snd_una = ack;
			t->snd_wnd = t->wscaleOk ? ((uint32_t) wnd << t->sndWscale) : wnd;   // RFC 7323: scaled after handshake
			t->dupacks = 0;
			onAckCwnd(t, acked);
			// RTT: prefer the RFC 7323 §4.1 timestamp sample (every ack carries the echo) over the
			// single Karn-timer probe; fall back to the timer when timestamps aren't in use.
			if (t->tsOk && peerHasTs && peerTsEcr) { int r = (int)(now() - peerTsEcr); if (r >= 0) rttUpdate(t, r); t->rttPending = false; }
			else if (t->rttPending && seqGeq(ack, t->rttSeq)) { rttUpdate(t, (int)(now() - t->rttStart)); t->rttPending = false; }
			if (t->snd_una == t->snd_nxt) t->rtoDeadline = 0;          // all acked: stop RTO
			else armRto(t);
			// state transitions on our FIN being acked
			if (t->state == TCP_FIN_WAIT_1 && t->finSent && seqGt(ack, t->finSeq)) t->state = TCP_FIN_WAIT_2;
			else if (t->state == TCP_CLOSING && seqGt(ack, t->finSeq)) enterTimeWait(t);
			else if (t->state == TCP_LAST_ACK && seqGt(ack, t->finSeq)) { t->state = TCP_CLOSED; tcbFree(t); netbufFree(skb); return; }
			sendData(t);   // window may have opened
		} else if (ack == t->snd_una && t->snd_una != t->snd_nxt && plen == 0 && !(flags & (TCP_SYN|TCP_FIN))) {
			// duplicate ACK -> fast retransmit on the 3rd
			if (++t->dupacks == 3) {
				t->ssthresh = (t->snd_nxt - t->snd_una) / 2;
				if (t->ssthresh < (uint32_t) 2 * t->mss) t->ssthresh = 2 * t->mss;
				t->cwnd = t->ssthresh + 3 * t->mss;
				// Retransmit the first un-SACKed chunk (the hole), not blindly from snd_una.
				uint32_t rseq; int rlen = retxChunk(t, &rseq);
				if (rlen > 0) sendSeg(t, TCP_ACK, rseq, t->sndBuf + (rseq - t->snd_una), rlen);
				armRto(t);
			}
		}
		if (t->state == TCP_SYN_RCVD) {                               // handshake completed (passive)
			t->state = TCP_ESTABLISHED;
			if (t->parent && t->parent->acceptCount < ACCEPT_N) {
				t->parent->acceptq[t->parent->acceptCount++] = t;
				sockWake(t->parent);
			}
		}
	}

	// data
	if (plen > 0 && (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2)) {
		bool inOrder = (seq == t->rcv_nxt);
		if (inOrder) {
			int got = rcvAppend(t, payload, plen);
			t->rcv_nxt += got;
			drainOoo(t);
			sockWake(t);
		} else if (seqGt(seq, t->rcv_nxt) && seqLt(seq, t->rcv_nxt + RCVBUF)) {
			queueOoo(t, seq, payload, plen);
		}
		bool hole = false;
		for (int i = 0; i < OOO_N; i++) if (t->ooo[i].used) { hole = true; break; }
		// Delayed ACK (Linux): hold an in-order ack up to DELAY_ACK, but ack IMMEDIATELY on an
		// out-of-order/hole-filling segment (the peer needs the dup-ack to fast-retransmit) or on
		// every second full segment; otherwise arm the timer so tcpTick flushes it.
		if (!inOrder || hole || ++t->ackSegs >= 2)
			sendSeg(t, TCP_ACK, t->snd_nxt, 0, 0);
		else { t->ackPending = true; t->ackDeadline = now() + DELAY_ACK; }
	}

	// FIN
	if ((flags & TCP_FIN) && seqLeq(seq, t->rcv_nxt)) {
		if (!t->peerFin) {
			t->peerFin = true;
			t->rcv_nxt += 1;
			sendSeg(t, TCP_ACK, t->snd_nxt, 0, 0);
			sockWake(t);
			if (t->state == TCP_ESTABLISHED) t->state = TCP_CLOSE_WAIT;
			else if (t->state == TCP_FIN_WAIT_2) enterTimeWait(t);
			else if (t->state == TCP_FIN_WAIT_1) {                    // simultaneous close
				if (t->finSent && seqGt(t->snd_una, t->finSeq)) enterTimeWait(t);
				else t->state = TCP_CLOSING;
			}
		}
	}
	netbufFree(skb);
}

void tcpTick(unsigned t_now) {
	for (int i = 0; i < TCB_N; i++) {
		Tcb* t = &g_tcbs[i];
		if (!t->used) continue;
		// Flush an owed delayed ACK once its deadline passes (Linux: the ack the peer is waiting on).
		if (t->ackPending && (int)(t_now - t->ackDeadline) >= 0)
			sendSeg(t, TCP_ACK, t->snd_nxt, 0, 0);   // clears ackPending in sendSeg
		// Zero-window persist probe: send one byte (without advancing snd_nxt) to force the peer to
		// re-advertise its window, backing off 5s -> 60s, until it reopens. Without this a stalled
		// window would hang the session forever.
		if (t->persistDeadline && (int)(t_now - t->persistDeadline) >= 0) {
			int off = (int) (t->snd_nxt - t->snd_una);
			if (t->snd_wnd == 0 && off < t->sndLen) {
				sendSeg(t, TCP_ACK, t->snd_nxt, t->sndBuf + off, 1);   // probe byte; peer (re)acks its window
				t->persistBackoff *= 2; if (t->persistBackoff > PERSIST_MAX) t->persistBackoff = PERSIST_MAX;
				t->persistDeadline = t_now + t->persistBackoff;
			} else {
				t->persistDeadline = 0;   // window opened or nothing to probe
			}
		}
		// Keepalive (RFC 1122 §4.2.3.6): after keepIdle of silence, probe with an old-seq ACK every
		// keepIntvl; after keepCnt unanswered probes declare the peer dead (ETIMEDOUT).
		if (t->keepalive && t->state == TCP_ESTABLISHED && t->keepDeadline && (int)(t_now - t->keepDeadline) >= 0) {
			if (t->keepProbes >= t->keepCnt) {
				if (t->sock) t->sock->soError = SOCK_ETIMEDOUT;
				t->state = TCP_CLOSED; t->keepDeadline = 0; sockWake(t);
				continue;
			}
			sendSeg(t, TCP_ACK, t->snd_una - 1, 0, 0);   // probe: stale seq -> the peer must ACK
			t->keepProbes++;
			t->keepDeadline = t_now + t->keepIntvl;
		}
		if (t->state == TCP_TIME_WAIT && t->timeWaitDeadline && (int)(t_now - t->timeWaitDeadline) >= 0) {
			t->state = TCP_CLOSED; tcbFree(t); continue;
		}
		if (t->rtoDeadline && (int)(t_now - t->rtoDeadline) >= 0 && t->snd_una != t->snd_nxt) {
			// Retransmission timeout: shrink the window (Reno), retransmit the oldest segment.
			t->ssthresh = (t->snd_nxt - t->snd_una) / 2;
			if (t->ssthresh < (uint32_t) 2 * t->mss) t->ssthresh = 2 * t->mss;
			t->cwnd = t->mss;
			t->dupacks = 0;
			t->rttPending = false;                  // Karn: don't sample a retransmitted segment
			t->rto *= 2; if (t->rto > RTO_MAX) t->rto = RTO_MAX;
			// A half-open handshake (our SYN / SYN-ACK is the only unacked segment) retransmits the
			// SYN itself — without this the timer fired but sent nothing, so a slow or lost SYN-ACK
			// hung the connect forever. Give up after SYN_RETRIES_MAX so connect() returns ETIMEDOUT.
			if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD) {
				if (++t->rtxCount > SYN_RETRIES_MAX) {
					if (t->sock) t->sock->soError = SOCK_ETIMEDOUT;
					t->state = TCP_CLOSED;
					t->rtoDeadline = 0;
					sockWake(t);
					continue;
				}
				sendSeg(t, t->state == TCP_SYN_SENT ? TCP_SYN : (uint8_t)(TCP_SYN | TCP_ACK), t->iss, 0, 0);
				t->rtoDeadline = t_now + t->rto;
				continue;
			}
			uint32_t rseq; int chunk = retxChunk(t, &rseq);   // skip SACKed ranges on RTO too
			if (chunk > 0) sendSeg(t, TCP_ACK | TCP_PSH, rseq, t->sndBuf + (rseq - t->snd_una), chunk);
			else if (t->finSent) sendSeg(t, TCP_ACK | TCP_FIN, t->finSeq, 0, 0);
			t->rtoDeadline = t_now + t->rto;
		}
	}
}

// ---- socket integration ----
int tcpAttach(Socket* s) {
	Tcb* t = tcbAlloc();
	if (!t) return -SOCK_ENOBUFS;
	t->sock = s; s->tcp = t;
	return 0;
}

int tcpConnect(Socket* s, uint32_t ip, uint16_t port) {
	if (!s || !s->tcp) return -SOCK_EINVAL;
	Tcb* t = (Tcb*) s->tcp;
	if (t->state != TCP_CLOSED) return -SOCK_EISCONN;
	NetDevice* dev = 0; uint32_t nh = 0;
	if (!routeLookup(ip, &dev, &nh) || !dev) return -SOCK_ENOBUFS;
	t->localIp = dev->ip;
	t->remoteIp = ip; t->remotePort = port;
	if (!s->bound) { s->localPort = socketEphemeralPort(); s->bound = true; }
	t->localPort = s->localPort;
	t->iss = g_isnCounter; g_isnCounter += 0x4000;
	t->snd_una = t->iss; t->snd_nxt = t->iss + 1;
	t->state = TCP_SYN_SENT;
	t->rtxCount = 0;
	g_netStats.tcpActiveOpens++;
	sendSeg(t, TCP_SYN, t->iss, 0, 0);
	armRto(t);
	return 0;
}

void tcpIcmpError(uint32_t localIp, uint16_t localPort, uint32_t remoteIp, uint16_t remotePort, int err) {
	(void) localIp;   // a host has one local IP per route; the 4-tuple ports+peer pin the connection
	for (int i = 0; i < TCB_N; i++) {
		Tcb* t = &g_tcbs[i];
		if (!t->used) continue;
		if (t->localPort != localPort || t->remotePort != remotePort || t->remoteIp != remoteIp) continue;
		// SYN_SENT/SYN_RCVD: a hard error aborts the half-open connect (mirror the RST path).
		// ESTABLISHED and later: ignore soft ICMP errors, exactly as Linux does — a transient
		// unreachable must not tear down a working connection.
		if (t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD) {
			if (t->sock) t->sock->soError = err;
			t->state = TCP_CLOSED;
			t->rtoDeadline = 0;
			sockWake(t);
		}
		return;
	}
}

int tcpSend(Socket* s, const void* buf, unsigned len) {
	if (!s || !s->tcp) return -SOCK_EINVAL;
	Tcb* t = (Tcb*) s->tcp;
	if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) return -SOCK_ENOTCONN;
	int free = SNDBUF - t->sndLen;
	if (free <= 0) return -SOCK_EAGAIN;                  // send buffer full (dispatch may block)
	int n = (int) len < free ? (int) len : free;
	memcpy(t->sndBuf + t->sndLen, buf, n);
	t->sndLen += n;
	sendData(t);
	return n;
}

int tcpRecv(Socket* s, void* buf, unsigned len, int flags) {
	if (!s || !s->tcp) return -SOCK_EINVAL;
	Tcb* t = (Tcb*) s->tcp;
	if (t->rcvCount == 0) {
		if (t->peerFin || t->state == TCP_CLOSE_WAIT || t->state == TCP_CLOSED) return 0;  // EOF
		return -SOCK_EAGAIN;
	}
	int n = (int) len < t->rcvCount ? (int) len : t->rcvCount;
	unsigned char* d = (unsigned char*) buf;
	int tail = t->rcvTail;
	for (int i = 0; i < n; i++) { d[i] = t->rcvBuf[tail]; tail = (tail + 1) % RCVBUF; }
	if (!(flags & MSG_PEEK)) { t->rcvTail = tail; t->rcvCount -= n; }
	return n;
}

void tcpClose(Socket* s) {
	if (!s || !s->tcp) return;
	Tcb* t = (Tcb*) s->tcp;
	t->sock = 0;                                          // orphan: TCB finishes closing on its own
	s->tcp = 0;
	// Flush any buffered-but-unsent payload BEFORE the FIN, so close() never drops queued data
	// (e.g. an HTTP response body the app wrote just before closing). At close there is no more
	// data coming, so Nagle's "wait for a full segment" assumption is moot — force nodelay so the
	// flush emits a small final segment even while the previous one is still unacked (otherwise the
	// body races the FIN and is lost). The FIN's sequence then sits after all data the peer must
	// still receive. sendData transmits as much as the window allows; for the common small-response
	// case that is everything, so snd_nxt reaches snd_una+sndLen.
	t->nodelay = true;
	sendData(t);
	switch (t->state) {
	case TCP_ESTABLISHED:
		t->finSeq = t->snd_nxt;
		sendSeg(t, TCP_ACK | TCP_FIN, t->snd_nxt, 0, 0);
		t->snd_nxt += 1; t->finSent = true; t->state = TCP_FIN_WAIT_1; armRto(t);
		break;
	case TCP_CLOSE_WAIT:
		t->finSeq = t->snd_nxt;
		sendSeg(t, TCP_ACK | TCP_FIN, t->snd_nxt, 0, 0);
		t->snd_nxt += 1; t->finSent = true; t->state = TCP_LAST_ACK; armRto(t);
		break;
	case TCP_SYN_SENT:
	case TCP_LISTEN:
	default:
		t->state = TCP_CLOSED; tcbFree(t);
		break;
	}
}

// shutdown(2): SHUT_WR (1) / SHUT_RDWR (2) send our FIN but — unlike close() — keep the TCB
// attached so the app can still read. SHUT_RD (0) is a no-op (we just stop being a reader).
void tcpShutdown(Socket* s, int how) {
	if (!s || !s->tcp || how == 0) return;
	Tcb* t = (Tcb*) s->tcp;
	if (t->finSent) return;
	if (t->state == TCP_ESTABLISHED) {
		t->finSeq = t->snd_nxt;
		sendSeg(t, TCP_ACK | TCP_FIN, t->snd_nxt, 0, 0);
		t->snd_nxt += 1; t->finSent = true; t->state = TCP_FIN_WAIT_1; armRto(t);
	} else if (t->state == TCP_CLOSE_WAIT) {
		t->finSeq = t->snd_nxt;
		sendSeg(t, TCP_ACK | TCP_FIN, t->snd_nxt, 0, 0);
		t->snd_nxt += 1; t->finSent = true; t->state = TCP_LAST_ACK; armRto(t);
	}
}

int tcpListen(Socket* s, int /*backlog*/) {
	if (!s || !s->tcp) return -SOCK_EINVAL;
	Tcb* t = (Tcb*) s->tcp;
	if (!s->bound) return -SOCK_EINVAL;
	t->localIp = s->localIp; t->localPort = s->localPort;
	t->isListen = true; t->state = TCP_LISTEN;
	return 0;
}

Socket* tcpAccept(Socket* s, int* err) {
	if (!s || !s->tcp) { if (err) *err = -SOCK_EINVAL; return 0; }
	Tcb* t = (Tcb*) s->tcp;
	if (!t->isListen || t->acceptCount == 0) { if (err) *err = -SOCK_EAGAIN; return 0; }
	Tcb* c = t->acceptq[0];
	for (int i = 1; i < t->acceptCount; i++) t->acceptq[i - 1] = t->acceptq[i];
	t->acceptCount--;
	Socket* ns = g_newSock ? g_newSock(AF_INET, SOCK_STREAM, 0) : 0;
	if (!ns) { if (err) *err = -SOCK_ENOBUFS; return 0; }
	// Re-point: the new socket adopts the established child TCB (drop the auto-attached one).
	if (ns->tcp && ns->tcp != c) tcbFree((Tcb*) ns->tcp);
	ns->tcp = c; c->sock = ns;
	ns->connected = true; ns->localPort = c->localPort; ns->remoteIp = c->remoteIp; ns->remotePort = c->remotePort;
	if (err) *err = 0;
	return ns;
}

bool tcpReadable(Socket* s) {
	if (!s || !s->tcp) return false;
	Tcb* t = (Tcb*) s->tcp;
	if (t->isListen) return t->acceptCount > 0;
	return t->rcvCount > 0 || t->peerFin || t->state == TCP_CLOSE_WAIT || t->state == TCP_CLOSED;
}
bool tcpWritable(Socket* s) {
	if (!s || !s->tcp) return false;
	Tcb* t = (Tcb*) s->tcp;
	return (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) && t->sndLen < SNDBUF;
}
int tcpState(Socket* s) { return (s && s->tcp) ? ((Tcb*) s->tcp)->state : TCP_CLOSED; }
uint32_t tcpSndWnd(Socket* s) { return (s && s->tcp) ? ((Tcb*) s->tcp)->snd_wnd : 0; }

void tcpKeepalive(Socket* s, bool on) {
	if (!s || !s->tcp) return;
	Tcb* t = (Tcb*) s->tcp;
	t->keepalive = on; t->keepProbes = 0;
	t->keepDeadline = on ? now() + t->keepIdle : 0;
}

void tcpNodelay(Socket* s, bool on) {
	if (!s || !s->tcp) return;
	Tcb* t = (Tcb*) s->tcp;
	t->nodelay = on;
	if (on) sendData(t);   // flush anything Nagle was holding
}
void tcpKeepParam(Socket* s, int name, int seconds) {
	if (!s || !s->tcp || seconds <= 0) return;
	Tcb* t = (Tcb*) s->tcp;
	if (name == TCP_KEEPIDLE)       { t->keepIdle = (unsigned) seconds * 1000;
		if (t->keepalive) t->keepDeadline = now() + t->keepIdle; }   // re-arm if already enabled
	else if (name == TCP_KEEPINTVL) t->keepIntvl = (unsigned) seconds * 1000;
	else if (name == TCP_KEEPCNT)   t->keepCnt   = seconds;        // a count, not seconds
}

int tcpSnapshot(TcpConnInfo* out, int max) {
	int n = 0;
	for (int i = 0; i < TCB_N && n < max; i++) {
		Tcb* t = &g_tcbs[i];
		if (!t->used) continue;
		out[n].localIp    = t->localIp;
		out[n].localPort  = t->localPort;
		out[n].remoteIp   = t->remoteIp;
		out[n].remotePort = t->remotePort;
		out[n].state      = t->state;
		out[n].txQueue    = t->sndLen;
		out[n].rxQueue    = t->rcvCount;
		n++;
	}
	return n;
}

void tcpReset() {
	for (int i = 0; i < TCB_N; i++) g_tcbs[i].used = false;
	g_clock = 0; g_isnCounter = 0x10000;
}

}  // namespace kernel
