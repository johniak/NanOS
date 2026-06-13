/*
 * Packet.h — AF_PACKET sockets (FAZA F): a tap on the L2 RX path + raw L2 TX, the mechanism a
 * DHCP client needs to exchange frames on an interface that has NO IP address yet.
 *
 *   SOCK_RAW   — the application sees / supplies the FULL Ethernet frame.
 *   SOCK_DGRAM — "cooked": the kernel strips the Ethernet header on RX and builds it on TX from
 *                the sockaddr_ll (dst MAC + ifindex + protocol).
 *
 * ifindex here is 1-based: the device registry slot + 1 (slot 0 -> ifindex 1), so 0 is "invalid".
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct Socket;
struct NetBuf;
struct NetDevice;

// ifindex <-> device. netIfIndexOf returns 0 if the device is not registered.
int        netIfIndexOf(NetDevice* dev);
NetDevice* netByIfIndex(int idx);

// AF_PACKET socket ops — invoked from the socket syscall layer when the socket's domain is
// AF_PACKET (the sockaddr_ll fields are unpacked by that layer).
int packetBind(Socket* s, int ifindex, uint16_t protocol /* network order; 0 keeps the current */);
int packetSend(Socket* s, const void* buf, unsigned len, int ifindex, uint16_t protocol,
               const unsigned char* dmac);
int packetRecv(Socket* s, void* buf, unsigned len, int flags,
               int* ifindexOut, uint16_t* protoOut /* network order */, int* pkttypeOut,
               unsigned char macOut[8]);

// True if a datagram is queued (so the syscall layer's poll/blocking logic works uniformly).
bool packetReadable(Socket* s);

// RX tap: ethRx calls this with the FULL frame (head() at the Ethernet header) BEFORE the L3
// demux, delivering a copy to every matching AF_PACKET socket. Never consumes the frame.
void packetRxTap(NetBuf* frame);

}  // namespace kernel
