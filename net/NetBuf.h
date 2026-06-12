/*
 * NetBuf.h — the sk_buff of NanOS: one packet buffer with headroom, so each layer can
 * prepend its header (push) on TX and strip it (pull) on RX without copying. Layout mirrors
 * Linux's head/data/tail/end:
 *
 *     buf[0] ........ data .......... data+len ............ CAP
 *     |  headroom   |   valid bytes  |     tailroom        |
 *
 * TX builds bottom-up: reserve() headroom, put() the payload at the tail, then each lower
 * layer push()es its header in front. RX strips top-down: pull() past each header.
 *
 * Buffers come from a fixed pool (no per-packet malloc on the hot path). MI + host-testable.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct NetDevice;

struct NetBuf {
	// 2 KiB holds a full 1514-byte Ethernet frame plus generous headroom for stack-built
	// headers (Ethernet 14 + IP 60 + TCP 60). Matches the e1000's 2 KiB RX buffers.
	static const int CAP = 2048;

	unsigned char buf[CAP];
	int      data;        // offset of the first valid byte
	int      len;         // number of valid bytes  (tail = data + len)
	NetDevice* dev;       // ingress (RX) or egress (TX) device
	uint16_t protocol;    // ethertype in host order, set by the Ethernet demux on RX
	int      l3;          // offset of the L3 (IP) header, set during RX parse (-1 = unset)
	int      l4;          // offset of the L4 (TCP/UDP/ICMP) header (-1 = unset)
	uint32_t saddr;       // IPv4 source (host order), set by the IP demux for the transport layer
	uint32_t daddr;       // IPv4 destination (host order)
	uint8_t  ipproto;     // IP protocol number (1 ICMP / 6 TCP / 17 UDP)

	unsigned char* head() { return buf + data; }
	unsigned char* tail() { return buf + data + len; }
	int headroom() const { return data; }
	int tailroom() const { return CAP - (data + len); }

	// Set the initial headroom (call once on a fresh TX buffer before building).
	void reserve(int n) { data = n; len = 0; }

	// Prepend n bytes (a header) in front of the data; returns the new data pointer.
	unsigned char* push(int n) { data -= n; len += n; return buf + data; }
	// Strip n bytes from the front (consume a header); returns the new data pointer.
	unsigned char* pull(int n) { data += n; len -= n; return buf + data; }
	// Append n bytes at the tail (payload); returns a pointer to the appended region.
	unsigned char* put(int n) { unsigned char* p = buf + data + len; len += n; return p; }
	// Shrink the data to exactly newLen bytes (drop trailing).
	void trim(int newLen) { if (newLen < len) len = newLen; }
};

// Pool allocator. alloc() returns a zeroed-metadata buffer with data=0/len=0 (caller
// reserve()s headroom for TX, or fills head()/len for RX). free() returns it to the pool.
// alloc() returns 0 if the pool is exhausted (caller must drop the packet, never block here).
NetBuf* netbufAlloc();
void    netbufFree(NetBuf* b);

// Pool statistics (for /proc/net + leak detection in tests).
int netbufInUse();
int netbufCapacity();

// IRQ guard: the pool is touched from both the driver IRQ (alloc, in knx_netif_rx) and the
// softirq thread (free). The kernel installs cpuIrqSave/Restore so the free-list can't be
// corrupted by an IRQ landing mid-update. Host tests are single-threaded and leave it unset.
typedef unsigned long (*NetbufIrqSaveFn)();
typedef void (*NetbufIrqRestoreFn)(unsigned long);
void netbufSetIrqGuard(NetbufIrqSaveFn save, NetbufIrqRestoreFn restore);

}  // namespace kernel
