#include "NetDevice.h"

namespace kernel {

namespace {
const int MAX_DEV = 8;
NetDevice* g_devs[MAX_DEV];
int        g_ndev = 0;

// RX backlog: a ring of skb pointers between the driver IRQ (producer) and the softirq thread
// (consumer). Guarded by the IRQ hooks so an IRQ can't corrupt the ring mid-dequeue.
const int BACKLOG = 64;     // < pool size, so the backlog (not the pool) is the drop point
NetBuf*   g_backlog[BACKLOG];
int       g_blHead = 0, g_blTail = 0, g_blCount = 0;

NetInputFn      g_input   = 0;
NetWakeFn       g_wake    = 0;
NetIrqSaveFn    g_irqSave = 0;
NetIrqRestoreFn g_irqRestore = 0;

inline unsigned long lock()    { return g_irqSave ? g_irqSave() : 0; }
inline void unlock(unsigned long f) { if (g_irqRestore) g_irqRestore(f); }
}  // namespace

bool netRegister(NetDevice* dev) {
	if (!dev || g_ndev >= MAX_DEV)
		return false;
	g_devs[g_ndev++] = dev;
	return true;
}

static bool nameEq(const char* a, const char* b) {
	for (int i = 0; i < 16; i++) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
	return true;
}

NetDevice* netByName(const char* name) {
	for (int i = 0; i < g_ndev; i++)
		if (nameEq(g_devs[i]->name, name))
			return g_devs[i];
	return 0;
}
NetDevice* netByIndex(int i) { return (i >= 0 && i < g_ndev) ? g_devs[i] : 0; }
int        netCount()        { return g_ndev; }

NetDevice* netPrimary() {
	for (int i = 0; i < g_ndev; i++)
		if (!(g_devs[i]->flags & NETIF_LOOPBACK) && (g_devs[i]->flags & NETIF_UP))
			return g_devs[i];
	return 0;
}

int netTransmit(NetDevice* dev, NetBuf* skb) {
	if (!dev || !skb) { if (skb) netbufFree(skb); return -1; }
	if (!dev->tx) { dev->txDropped++; netbufFree(skb); return -1; }
	int n = skb->len;
	int rc = dev->tx(dev, skb);
	if (rc == 0) { dev->txPackets++; dev->txBytes += (uint64_t) n; }
	else         { dev->txErrors++; }
	// dev->tx owns the skb lifetime on success (loopback re-queues it; the e1000 copies into the
	// DMA ring then frees it). On error netTransmit frees to avoid a leak.
	if (rc != 0)
		netbufFree(skb);
	return rc;
}

int netifRx(NetBuf* skb) {
	if (!skb)
		return -1;
	unsigned long f = lock();
	if (g_blCount >= BACKLOG) {
		unlock(f);
		if (skb->dev) skb->dev->rxDropped++;
		netbufFree(skb);
		return -1;
	}
	g_backlog[g_blHead] = skb;
	g_blHead = (g_blHead + 1) % BACKLOG;
	g_blCount++;
	unlock(f);
	if (g_wake)
		g_wake();
	return 0;
}

bool netRxPending() {
	unsigned long f = lock();
	bool any = g_blCount > 0;
	unlock(f);
	return any;
}

int netRxProcess() {
	int processed = 0;
	for (;;) {
		unsigned long f = lock();
		if (g_blCount == 0) { unlock(f); break; }
		NetBuf* skb = g_backlog[g_blTail];
		g_blTail = (g_blTail + 1) % BACKLOG;
		g_blCount--;
		unlock(f);

		if (skb->dev) { skb->dev->rxPackets++; skb->dev->rxBytes += (uint64_t) skb->len; }
		if (g_input)
			g_input(skb);       // handler owns the skb (frees or forwards)
		else
			netbufFree(skb);    // no stack wired yet (early boot / unit test) — drop
		processed++;
	}
	return processed;
}

void netSetInputHandler(NetInputFn fn) { g_input = fn; }
void netSetWakeFn(NetWakeFn fn)        { g_wake = fn; }
void netSetIrqGuard(NetIrqSaveFn save, NetIrqRestoreFn restore) { g_irqSave = save; g_irqRestore = restore; }

void netReset() {
	// Free any queued buffers so tests don't leak the pool.
	while (g_blCount > 0) {
		netbufFree(g_backlog[g_blTail]);
		g_blTail = (g_blTail + 1) % BACKLOG;
		g_blCount--;
	}
	g_ndev = 0; g_blHead = g_blTail = g_blCount = 0;
	g_input = 0; g_wake = 0; g_irqSave = 0; g_irqRestore = 0;
}

}  // namespace kernel
