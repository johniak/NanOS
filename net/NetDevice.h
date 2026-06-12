/*
 * NetDevice.h — the MI network-device abstraction + RX bottom-half, the seam between drivers
 * (e1000 kext, loopback) and the protocol stack.
 *
 *   driver IRQ  --netifRx(skb)-->  [backlog queue]  --netRxProcess()-->  L2 input (eth_rx)
 *                  (enqueue+wake)                     (kernel softirq thread, NOT in IRQ)
 *
 * Per deferred-preemption, the driver IRQ only enqueues + wakes; all protocol work runs in a
 * dedicated kernel thread that calls netRxProcess(). The queue/drain are pure and host-testable;
 * the kernel installs wake + IRQ-guard hooks so the MI core needs no scheduler/arch dependency.
 */
#pragma once
#include <stdint.h>
#include "NetBuf.h"

namespace kernel {

enum NetFlags {
	NETIF_UP        = 0x01,   // administratively up
	NETIF_RUNNING   = 0x02,   // carrier/link present
	NETIF_LOOPBACK  = 0x04,
	NETIF_BROADCAST = 0x08,   // supports L2 broadcast (Ethernet)
};

struct NetDevice {
	char     name[16];        // "eth0", "lo"
	unsigned char mac[6];
	uint16_t mtu;             // L3 payload limit (1500 for Ethernet)
	uint32_t flags;

	// L3 config (host byte order). Routing proper lives in net/Route (FAZA 5); these are the
	// interface's primary address, set by DHCP/static config.
	uint32_t ip;
	uint32_t netmask;
	uint32_t broadcast;

	// Transmit one fully-built L2 frame. Returns 0 on success, <0 on error. Set by the driver
	// (e1000) or loopback. The stack calls dev->tx after building Ethernet+payload into skb.
	int (*tx)(NetDevice* dev, NetBuf* skb);
	void* drvCtx;             // opaque driver context (e.g. e1000 register base + ring state)

	// Statistics (Linux /proc/net/dev style).
	uint64_t rxPackets, rxBytes, rxErrors, rxDropped;
	uint64_t txPackets, txBytes, txErrors, txDropped;
};

// ---- device registry -------------------------------------------------------
// Register a device (the kernel allocates the struct; the registry keeps the pointer).
bool      netRegister(NetDevice* dev);
NetDevice* netByName(const char* name);
NetDevice* netByIndex(int i);     // 0..netCount()-1, for iteration / /proc/net/dev
int        netCount();
// The first non-loopback UP device (the default egress until routing is configured).
NetDevice* netPrimary();

// Transmit helper: bumps tx stats and calls dev->tx. Frees the skb on driver error.
int netTransmit(NetDevice* dev, NetBuf* skb);

// ---- RX bottom-half --------------------------------------------------------
// Called by a driver (from IRQ context, via knx_netif_rx): enqueue the received frame and wake
// the softirq thread. skb->dev must be set. If the backlog is full the frame is dropped (stat
// bumped) — never blocks. Returns 0 if queued, <0 if dropped.
int netifRx(NetBuf* skb);

// Drain the backlog, delivering each frame to the installed L2 input handler. Runs in the
// softirq thread (never IRQ). Returns the number of frames processed. Host tests call it directly.
int netRxProcess();

// True if the backlog has frames waiting (the softirq thread's wait condition).
bool netRxPending();

// The L2 input handler (Ethernet installs eth_rx in FAZA 4; tests install a capture). The
// handler owns the skb and must free it (or hand it on). Exactly one handler.
typedef void (*NetInputFn)(NetBuf* skb);
void netSetInputHandler(NetInputFn fn);

// Kernel-installed hooks so the MI core stays arch/scheduler-free:
//   wake  — wake the softirq thread after an enqueue (Scheduler::wakeAll on the net wait queue);
//   guard — disable/enable interrupts around the (short) backlog critical section.
typedef void (*NetWakeFn)();
typedef unsigned long (*NetIrqSaveFn)();
typedef void (*NetIrqRestoreFn)(unsigned long);
void netSetWakeFn(NetWakeFn fn);
void netSetIrqGuard(NetIrqSaveFn save, NetIrqRestoreFn restore);

// Test/utility: reset the registry, backlog and handler to a clean state.
void netReset();

}  // namespace kernel
