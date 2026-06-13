/*
 * Unix.h — AF_UNIX (local) sockets: the MI core behind socket(AF_UNIX, ...). Pure software, so it
 * is host-tested like the rest of net/ (no VFS/scheduler dependency — the kernel injects the wake
 * hook via socketSetWakeFn, exactly as the IP path does).
 *
 * Two transports share one Socket object (it lives in net/Socket.cpp's g_socks table, so refcount,
 * fork/dup and close work uniformly):
 *   - SOCK_STREAM/SEQPACKET: a refcounted bidirectional byte channel (UnixChannel = two per-
 *     direction rings + the two endpoints, for waking the reader/blocked writer). connect/accept
 *     pair two endpoints onto one channel. Behaves like a two-way pipe.
 *   - SOCK_DGRAM: connectionless; sendto a bound path delivers one message into the peer's existing
 *     Socket RXQ ring (socketDeliver) — message boundaries preserved for free.
 *
 * Naming: bind() records a filesystem path on the socket; connect()/EADDRINUSE resolve it by
 * scanning g_socks (64 slots). The kernel additionally plants an S_IFSOCK node in the VFS for `ls`
 * visibility, but THIS layer is the authority for path->socket resolution so it stays host-testable.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct Socket;

static const unsigned UNIX_PATH_MAX = 108;   // sizeof(sockaddr_un.sun_path)

// Lifecycle hooks called from net/Socket.cpp (socketCreate / socketClose) for AF_UNIX sockets.
int  unixAttach(Socket* s);     // allocate per-socket state; <0 on OOM
void unixDetach(Socket* s);     // refs==0 teardown: wake peer, drop channel ref, unbind path, free

// Namespace ops. `path`/`pathLen` are the raw sun_path bytes (pathLen includes the trailing NUL for
// named sockets; an abstract socket — Linux extension — has path[0]==0 and is matched verbatim).
int  unixBind(Socket* s, const char* path, unsigned pathLen);
int  unixListen(Socket* s, int backlog);
int  unixConnect(Socket* s, const char* path, unsigned pathLen);   // STREAM: pair; DGRAM: set peer
Socket* unixAccept(Socket* s, int* err);                            // new server-side socket, or 0

// I/O. STREAM ignores path; DGRAM uses path (unconnected sendto) or the connected peer.
int  unixSend(Socket* s, const void* buf, unsigned len, const char* path, unsigned pathLen);
int  unixRecv(Socket* s, void* buf, unsigned len, int flags, char* fromPath, unsigned* fromLen);

bool unixReadable(const Socket* s);
bool unixWritable(const Socket* s);

// socketpair(AF_UNIX, SOCK_STREAM/SEQPACKET): two pre-connected sockets sharing one channel.
int  unixSocketpair(int type, int protocol, Socket** a, Socket** b);

// Bound name for getsockname / /proc. Returns pathLen (0 if unbound) and copies into out[cap].
unsigned unixGetName(const Socket* s, char* out, unsigned cap);
bool     unixIsListening(const Socket* s);
const Socket* unixPeerOf(const Socket* s);

void unixReset();   // tests: forget all bindings (sockets themselves are freed by socketReset)

}  // namespace kernel
