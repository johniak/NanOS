/*
 * NetCore.cpp — the kernel/driver bridge for networking: the knx_* net exports a driver kext
 * calls, plus the RX softirq thread. MI-clean: reaches hardware only through <arch/...>
 * contracts (mmu, cpu) and the global frame allocator.
 */
#include "NetCore.h"
#include "NetDevice.h"
#include "NetBuf.h"
#include "Loopback.h"
#include "Ether.h"
#include "Arp.h"
#include "Ip.h"
#include "Icmp.h"
#include "Socket.h"
#include "Udp.h"
#include "Raw.h"
#include "Tcp.h"
#include "Route.h"
#include "Net.h"
#include "WaitQueue.h"
#include "Scheduler.h"
#include "WaitQueue.h"
#include "FrameAllocator.h"
#include <arch/mmu.h>
#include <arch/cpu.h>
#include <string.h>

namespace kernel {

namespace {
WaitQueue g_netWq;            // the softirq thread parks here; netifRx's wake hook wakes it

// Per-registered-device bridge: remembers the kext's transmit callback + context. NetDevice.tx
// points at kextNetTx, which forwards the frame bytes to the kext and frees the skb (the device
// owns the skb per the ndo_start_xmit contract).
struct KextBridge {
	knx_tx_fn tx;
	void*     drvctx;
};

int kextNetTx(NetDevice* dev, NetBuf* skb) {
	KextBridge* br = (KextBridge*) dev->drvCtx;
	int rc = (br && br->tx) ? br->tx(br->drvctx, skb->head(), skb->len) : -1;
	netbufFree(skb);          // device owns the skb (the kext copied it into its DMA ring)
	return rc;
}

void netWake() { Scheduler::wakeAll(&g_netWq); }
void netSocketWake(WaitQueue* wq) { Scheduler::wakeAll(wq); }   // wake a blocked socket reader

bool netRxReady(void*) { return netRxPending(); }

unsigned netClock() { return Scheduler::ticks(); }

void netSoftirqBody() {
	for (;;) {
		Scheduler::sleepOnUntil(&g_netWq, netRxReady, 0);   // sleep until a frame is queued
		netRxProcess();                                     // drain + demux (never in IRQ)
	}
}

// Periodic timer thread: drives the protocol timers (TCP RTO/TIME-WAIT need ticks even when
// idle; ARP/IP-reasm aging too) on a fixed cadence. 50 ms granularity is well under RTO_MIN.
void netTimerBody() {
	for (;;) {
		unsigned t = Scheduler::ticks();
		tcpTick(t);
		arpTick(t);
		ipReasmTick(t);
		Scheduler::sleepUntil(t + 50);
	}
}
}  // namespace

// ---- driver-facing exports (also entries in the KernelExports table) -------------------------
extern "C" {

void* knx_map_mmio(uint32_t phys, uint32_t len) {
	arch::mmuMapKernelMmio(phys, len);
	return (void*) phys;      // identity-mapped: the physical address is directly usable
}

void* knx_dma_alloc(uint32_t len, uint32_t* phys_out) {
	if (len == 0 || len > FRAME_SIZE) {   // our rings (<=4 KiB) and 2 KiB buffers all fit one frame
		if (phys_out) *phys_out = 0;
		return 0;
	}
	// DMA memory is touched via phys==virt from ARBITRARY context: a NIC TX runs in the
	// sending process's syscall (user CR3), RX drain in whatever CR3 the interrupt hit.
	// A frame inside the privatized user window [VA_USER_BASE, VA_USER_END) is not identity
	// there (#PF on the first packet once the allocator's low frames run into the window —
	// exactly how ping regressed when boot-time allocations drifted past 40 MiB). Allocate
	// above the window: [VA_USER_END, ...) stays identity in every address space.
	uint32_t pa = (uint32_t) g_frames.allocAbove(arch::VA_USER_END);
	if (!pa) { if (phys_out) *phys_out = 0; return 0; }
	memset((void*) (uintptr_t) pa, 0, FRAME_SIZE);   // identity-mapped RAM: virt == phys
	if (phys_out) *phys_out = pa;
	return (void*) (uintptr_t) pa;
}

void* knx_add_net_dev(struct KnxNetDev* desc) {
	if (!desc)
		return 0;
	NetDevice* dev = new NetDevice;
	memset(dev, 0, sizeof(*dev));
	for (int i = 0; i < 16; i++) dev->name[i] = desc->name[i];
	for (int i = 0; i < 6; i++)  dev->mac[i]  = desc->mac[i];
	dev->mtu = desc->mtu ? desc->mtu : 1500;
	dev->flags = NETIF_UP | NETIF_RUNNING | NETIF_BROADCAST;
	KextBridge* br = new KextBridge;
	br->tx = desc->tx; br->drvctx = desc->drvctx;
	dev->drvCtx = br;
	dev->tx = kextNetTx;
	if (!netRegister(dev))
		return 0;
	return dev;
}

void knx_netif_rx(void* handle, const void* data, int len) {
	NetDevice* dev = (NetDevice*) handle;
	if (!dev || len <= 0 || len > NetBuf::CAP)
		return;
	NetBuf* skb = netbufAlloc();
	if (!skb) {               // pool exhausted: drop (the bottom-half is behind) — never block in IRQ
		if (dev) dev->rxDropped++;
		return;
	}
	skb->reserve(0);
	memcpy(skb->put(len), data, len);
	skb->dev = dev;
	netifRx(skb);             // enqueue + wake the softirq thread
}

}  // extern "C"

Task* netCoreInit() {
	netSetIrqGuard(arch::cpuIrqSave, arch::cpuIrqRestore);     // short backlog critical sections
	netbufSetIrqGuard(arch::cpuIrqSave, arch::cpuIrqRestore);  // pool alloc(IRQ)/free(thread) safety
	netSetWakeFn(netWake);
	NetDevice* lo = loopbackCreate();                        // lo, 127.0.0.1/8
	routeAdd(ipv4(127, 0, 0, 0), ipv4(255, 0, 0, 0), 0, lo, 0);  // 127/8 -> lo, else 127.0.0.1
	                                                         // falls through to the default route
	                                                         // (eth0/gw) and a self-connect is refused
	ethInit();                                               // ethRx becomes the L2 input handler
	arpInit();                                               // ARP receives via Ether
	ipInit();                                                // IP receives via Ether (ethertype 0x0800)
	icmpInit();                                               // ICMP echo reply + errors (IP proto 1)
	udpInit();                                                // UDP (IP proto 17) -> sockets
	rawInit();                                                // SOCK_RAW ICMP (ping)
	tcpInit();                                                // TCP (IP proto 6)
	socketSetWakeFn(netSocketWake);                           // wake blocked socket readers
	tcpSetNewSockHook(socketCreateRaw);                       // accept() mints a new socket
	arpSetClock(netClock);                                   // real ticks for neighbor aging
	ipReasmSetClock(netClock);                               // real ticks for fragment expiry
	tcpSetClock(netClock);                                   // real ticks for RTO/TIME-WAIT
	return Scheduler::create(netSoftirqBody, 2);             // ksoftirqd-net (task id 2)
}

// The periodic timer thread (id 3); the caller registers it as a kthread for /proc visibility.
Task* netTimerThread() { return Scheduler::create(netTimerBody, 3); }

// Interface bring-up: configure the primary NIC + default route. This is the STATIC fallback
// (QEMU user-net's well-known addresses); FAZA 10 runs DHCP and only falls back here. Safe to
// call once eth0 is registered (after the e1000 kext loads).
void netBringUp() {
	NetDevice* dev = netPrimary();
	if (!dev) return;
	dev->ip = ipv4(10, 0, 2, 15);
	dev->netmask = ipv4(255, 255, 255, 0);
	dev->broadcast = ipv4(10, 0, 2, 255);
	routeAddDefault(dev, ipv4(10, 0, 2, 2));   // on-link 10.0.2.0/24 + default via the gateway
}


}  // namespace kernel
