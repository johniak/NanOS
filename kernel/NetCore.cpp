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
		unsigned t = Scheduler::ticks();
		arpTick(t);                                         // traffic-driven neighbor aging
		ipReasmTick(t);                                     // expire incomplete fragment datagrams
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
	uint32_t pa = g_frames.alloc();
	if (!pa) { if (phys_out) *phys_out = 0; return 0; }
	memset((void*) pa, 0, FRAME_SIZE);    // identity-mapped RAM: virt == phys
	if (phys_out) *phys_out = pa;
	return (void*) pa;
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
	loopbackCreate();                                        // lo, 127.0.0.1/8
	ethInit();                                               // ethRx becomes the L2 input handler
	arpInit();                                               // ARP receives via Ether
	ipInit();                                                // IP receives via Ether (ethertype 0x0800)
	icmpInit();                                               // ICMP echo reply + errors (IP proto 1)
	udpInit();                                                // UDP (IP proto 17) -> sockets
	rawInit();                                                // SOCK_RAW ICMP (ping)
	socketSetWakeFn(netSocketWake);                           // wake blocked socket readers
	arpSetClock(netClock);                                   // real ticks for neighbor aging
	ipReasmSetClock(netClock);                               // real ticks for fragment expiry
	return Scheduler::create(netSoftirqBody, 2);             // ksoftirqd-net (task id 2)
}

}  // namespace kernel
