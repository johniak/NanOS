/*
 * Raw.h — SOCK_RAW (IPv4). The only raw protocol we need is ICMP, for ping: received ICMP is
 * delivered to raw sockets WITH the IP header (Linux raw-socket semantics — recvfrom returns
 * the full datagram), and rawSend hands the caller's ICMP message to ipOutput (the kernel adds
 * the IP header, as for a non-HDRINCL raw socket). rawInit() registers with ICMP's raw hook.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct Socket;

void rawInit();
int  rawSend(Socket* s, const void* buf, unsigned len, uint32_t dstIp, uint16_t dstPort);

}  // namespace kernel
