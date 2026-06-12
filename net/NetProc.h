/*
 * NetProc.h — Linux-style /proc/net renderers. Each fills `buf` (up to `cap` bytes) with the
 * same text format Linux exposes, reading live state from the net modules (NetDevice registry,
 * ARP cache, routing table, TCBs, sockets) via their introspection APIs. Returns the number of
 * bytes written. Pure w.r.t. the buffer (no I/O), so host-testable by populating module state.
 *
 * Address columns follow Linux's conventions: /proc/net/{tcp,udp,raw,route} print IPv4 in
 * network-byte-order read as a little-endian hex word (uppercase), ports as %04X; /proc/net/arp
 * prints dotted-decimal. TCP `st` uses Linux's own state numbering (not our enum order).
 */
#pragma once

namespace kernel {

int netProcDev(char* buf, int cap);     // /proc/net/dev   — per-interface RX/TX counters
int netProcRoute(char* buf, int cap);   // /proc/net/route — routing table
int netProcArp(char* buf, int cap);     // /proc/net/arp   — ARP cache
int netProcTcp(char* buf, int cap);     // /proc/net/tcp   — TCP connections
int netProcUdp(char* buf, int cap);     // /proc/net/udp   — UDP sockets
int netProcRaw(char* buf, int cap);     // /proc/net/raw   — RAW sockets
int netProcSnmp(char* buf, int cap);    // /proc/net/snmp  — protocol counters

}  // namespace kernel
