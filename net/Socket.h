/*
 * Socket.h — the MI socket layer: the struct socket/sock object, a datagram receive queue,
 * SO_* options, blocking-readiness, and a registry the transports (UDP/RAW) demux into. The
 * fd-table + syscall wiring (socketcall / direct syscalls) is FAZA 9; this core is driven and
 * host-tested directly. Addresses/ports are host byte order at this API.
 *
 * A socket is refcounted exactly like Pipe (kernel/Pipe.h): fork/dup share it, close drops a
 * reference, and the last close frees it — so an inherited socket behaves like Linux.
 */
#pragma once
#include <stdint.h>
#include "WaitQueue.h"

namespace kernel {

struct NetBuf;

// Linux i686 address-family / type / option constants (the wire+ABI values).
enum { AF_INET = 2, AF_PACKET = 17 };
enum { SOCK_STREAM = 1, SOCK_DGRAM = 2, SOCK_RAW = 3 };
enum {                              // setsockopt levels/names we honor
	SOL_SOCKET = 1,
	SO_REUSEADDR = 2, SO_TYPE = 3, SO_ERROR = 4, SO_BROADCAST = 6,
	SO_SNDBUF = 7, SO_RCVBUF = 8,
};
// recv/send flags
enum { MSG_PEEK = 0x02, MSG_DONTWAIT = 0x40 };

// errno values the socket layer returns (negated). Kept local so net/ needn't pull kernel/Syscall.h.
enum {
	SOCK_EAGAIN = 11, SOCK_EINVAL = 22, SOCK_EAFNOSUPPORT = 97, SOCK_ECONNREFUSED = 111,
	SOCK_EISCONN = 106, SOCK_ENOTCONN = 107, SOCK_EADDRINUSE = 98, SOCK_EMSGSIZE = 90,
	SOCK_EPROTONOSUPPORT = 93, SOCK_EACCES = 13, SOCK_ENOBUFS = 105,
};

struct Socket {
	int domain, type, protocol;
	uint32_t localIp, remoteIp;       // host order (0 = unspecified / INADDR_ANY)
	uint16_t localPort, remotePort;   // host order (0 = unbound)
	bool bound, connected;
	bool nonblock;
	bool broadcast;                   // SO_BROADCAST
	int  rcvbuf, sndbuf;              // SO_RCVBUF/SO_SNDBUF (advertised; cap enforced by the ring)
	int  soError;                     // pending SO_ERROR (e.g. async connect result, ICMP errors)
	int  refs;                        // open-fd references (fork/dup); freed at 0
	bool used;

	// datagram receive ring: each entry is a received message + its source address.
	static const int RXQ = 16;
	struct Dgram { NetBuf* skb; uint32_t srcIp; uint16_t srcPort; };
	Dgram rxq[RXQ];
	int rxHead, rxTail, rxCount, rxBytes;

	WaitQueue rxWait;                 // readers park here; deliver wakes them via the wake hook

	// TCP attaches its control block here (FAZA 8); 0 for UDP/RAW.
	void* tcp;
};

// Lifecycle. socketCreate validates the family/type (AF_INET + DGRAM/RAW now; STREAM in FAZA 8;
// AF_PACKET in FAZA 10) and returns a socket with refs=1, or 0 with *err set (negative errno).
Socket* socketCreate(int domain, int type, int protocol, int* err);
Socket* socketCreateRaw(int domain, int type, int protocol);   // err-less form (TCP accept hook)
void    socketRef(Socket* s);
void    socketClose(Socket* s);     // drop one reference; frees + unregisters at 0

int socketBind(Socket* s, uint32_t ip, uint16_t port);
int socketConnect(Socket* s, uint32_t ip, uint16_t port);
int socketGetSockName(Socket* s, uint32_t* ip, uint16_t* port);
int socketGetPeerName(Socket* s, uint32_t* ip, uint16_t* port);
int socketSetOpt(Socket* s, int level, int name, const void* val, unsigned len);
int socketGetOpt(Socket* s, int level, int name, void* val, unsigned* len);

// Send a datagram. dstIp/dstPort used when not connected (else the connected peer). Dispatches
// to UDP or RAW by type. Returns bytes sent or -errno.
int socketSendTo(Socket* s, const void* buf, unsigned len, uint32_t dstIp, uint16_t dstPort);

// Receive one datagram into buf (truncated to len). Fills srcIp/srcPort if non-null. MSG_PEEK
// leaves it queued. Returns bytes (datagram length, possibly truncated), 0, or -errno
// (-EAGAIN when empty + nonblock/MSG_DONTWAIT — the dispatch blocks on socketReadable instead).
int socketRecvFrom(Socket* s, void* buf, unsigned len, uint32_t* srcIp, uint16_t* srcPort, int flags);

// Readiness for poll/select and the blocking condition.
bool socketReadable(const Socket* s);
bool socketWritable(const Socket* s);
int  socketPoll(Socket* s);          // POLLIN/POLLOUT bits (CharDevice.h values)

// Transports call this to queue a received datagram on a socket (takes ownership of skb on
// success; returns false + leaves skb to the caller if the ring is full). Wakes blocked readers.
bool socketDeliver(Socket* s, NetBuf* skb, uint32_t srcIp, uint16_t srcPort);

// Registry lookup for the transports. UDP: most-specific match on (type, localPort, localIp,
// peer). RAW: match on (type, protocol). Returns 0 if none.
Socket* socketLookupUdp(uint32_t dstIp, uint16_t dstPort, uint32_t srcIp, uint16_t srcPort);
void    socketForEachRaw(int protocol, void (*fn)(Socket*, void*), void* ctx);

// Allocate an ephemeral local port (Linux range 32768..60999) not in use for DGRAM.
uint16_t socketEphemeralPort();

// Kernel installs a wake hook (Scheduler::wakeAll) so this MI core needs no scheduler dep.
typedef void (*SocketWakeFn)(WaitQueue*);
void socketSetWakeFn(SocketWakeFn fn);

// Wake a socket's blocked readers (used by TCP, which manages its own recv buffer rather than
// going through socketDeliver).
void socketWakeReaders(Socket* s);

void socketReset();   // tests: free all sockets + clear the registry

}  // namespace kernel
