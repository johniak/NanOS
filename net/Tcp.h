/*
 * Tcp.h — TCP: full state machine, sliding windows, RTO with Jacobson/Karn RTT estimation,
 * Reno congestion control (slow start / congestion avoidance / fast retransmit+recovery),
 * out-of-order reassembly, and proper close with TIME-WAIT (2 MSL). MSS option negotiated;
 * window scaling / SACK omitted (the plan marks them optional). tcpInit() registers tcpRx as
 * IP proto 6; tcpTick() drives the timers from the net-timer thread.
 *
 * Sockets integrate via the hooks below (called from net/Socket.cpp for SOCK_STREAM): the TCB
 * lives in this module's pool (so it can finish closing after the socket is freed — an orphaned
 * TIME-WAIT, like Linux), with a nullable back-pointer to its Socket.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct NetBuf;
struct Socket;

// TCP states (RFC 793).
enum TcpState {
	TCP_CLOSED = 0, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
	TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
};
// header flags
enum { TCP_FIN = 0x01, TCP_SYN = 0x02, TCP_RST = 0x04, TCP_PSH = 0x08, TCP_ACK = 0x10, TCP_URG = 0x20 };

void tcpInit();
void tcpRx(NetBuf* skb);
void tcpTick(unsigned now);
void tcpSetClock(unsigned (*fn)());

// ---- socket integration (SOCK_STREAM) ----
int  tcpAttach(Socket* s);                              // give a new STREAM socket a TCB
int  tcpConnect(Socket* s, uint32_t ip, uint16_t port); // active open (sends SYN)
int  tcpSend(Socket* s, const void* buf, unsigned len); // queue bytes; returns accepted or -errno
int  tcpRecv(Socket* s, void* buf, unsigned len, int flags);
void tcpClose(Socket* s);                               // active close (FIN); orphans the TCB
void tcpShutdown(Socket* s, int how);                   // shutdown(2): SHUT_WR sends FIN, keeps reading
int  tcpListen(Socket* s, int backlog);
Socket* tcpAccept(Socket* s, int* err);                 // dequeue a completed connection, or 0
bool tcpReadable(Socket* s);                            // data available or peer closed
bool tcpWritable(Socket* s);                            // ESTABLISHED with send-buffer room
int  tcpState(Socket* s);                               // for poll/tests

// Hook so a new passively-accepted connection can be wrapped in a Socket (Socket.cpp installs it).
typedef Socket* (*TcpNewSockFn)(int domain, int type, int protocol);
void tcpSetNewSockHook(TcpNewSockFn fn);

void tcpReset();   // tests

}  // namespace kernel
