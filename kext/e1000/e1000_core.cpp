#include "e1000_core.h"

// ---- register offsets (bytes from BAR0) ----
enum {
	REG_CTRL = 0x0000, REG_STATUS = 0x0008, REG_EERD = 0x0014,
	REG_ICR = 0x00C0, REG_ICS = 0x00C8, REG_IMS = 0x00D0, REG_IMC = 0x00D8,
	REG_RCTL = 0x0100, REG_TCTL = 0x0400, REG_TIPG = 0x0410,
	REG_RDBAL = 0x2800, REG_RDBAH = 0x2804, REG_RDLEN = 0x2808, REG_RDH = 0x2810, REG_RDT = 0x2818,
	REG_TDBAL = 0x3800, REG_TDBAH = 0x3804, REG_TDLEN = 0x3808, REG_TDH = 0x3810, REG_TDT = 0x3818,
	REG_RXCSUM = 0x5000, REG_MTA = 0x5200, REG_RAL0 = 0x5400, REG_RAH0 = 0x5404,
};
enum {
	CTRL_SLU = 0x00000040, CTRL_ASDE = 0x00000020, CTRL_RST = 0x04000000,
	RCTL_EN = 0x00000002, RCTL_BAM = 0x00008000, RCTL_SECRC = 0x04000000, RCTL_BSIZE_2048 = 0,
	TCTL_EN = 0x00000002, TCTL_PSP = 0x00000008,
	ICR_TXDW = 0x01, ICR_LSC = 0x04, ICR_RXDMT0 = 0x10, ICR_RXO = 0x40, ICR_RXT0 = 0x80,
	RXD_DD = 0x01, RXD_EOP = 0x02,
	TXD_EOP = 0x01, TXD_IFCS = 0x02, TXD_RS = 0x08, TXD_DD = 0x01,
	RAH_AV = 0x80000000,
	RXCSUM_IPOFL = 0x00000100, RXCSUM_TUOFL = 0x00000200,   // RX IP + TCP/UDP checksum offload enables
};

// ---- pure-logic helpers (compiled everywhere; host-tested) ----
int  ringNext(int cur, int n) { return (cur + 1) % n; }
bool rxDescDone(const RxDesc* d) { return (d->status & RXD_DD) != 0; }
void txEncode(TxDesc* d, uint32_t bufPhys, int len) {
	d->addr = bufPhys;
	d->length = (uint16_t) len;
	d->cso = 0;
	d->cmd = TXD_EOP | TXD_IFCS | TXD_RS;   // end-of-packet, insert FCS, report status
	d->status = 0;
}

#ifndef NANOS_HOST_TEST    // the engine half needs the knx_* kernel imports

#include "knx_net.h"   // KnxNetDev + knx_map_mmio/dma_alloc/add_net_dev/netif_rx

extern "C" int  knx_pci_find(uint16_t vendor, uint16_t device, uint8_t* bus, uint8_t* dev, uint8_t* func);
extern "C" uint64_t knx_pci_bar(uint8_t bus, uint8_t dev, uint8_t func, int n);
extern "C" uint8_t  knx_pci_irq(uint8_t bus, uint8_t dev, uint8_t func);
extern "C" void knx_pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func);
extern "C" void knx_register_irq(int irq, void (*h)(void*));
extern "C" int  knx_register_msi(uint8_t bus, uint8_t dev, uint8_t func, void (*h)(void*), void* ctx);
extern "C" void knx_log(const char* s);
extern "C" void* memcpy(void*, const void*, size_t);

static inline void wr(E1000Core* c, int off, uint32_t v) { *(volatile uint32_t*) (c->mmio + off) = v; }
static inline uint32_t rd(E1000Core* c, int off) { return *(volatile uint32_t*) (c->mmio + off); }

// Phase 1 runs a single NIC at a time (only the matching kext starts). The legacy-IRQ handler gets
// the trap frame (not the core) as its void* arg, and the MSI trampoline gets ctx — so both use this
// file-static set by coreStart rather than trusting the handler argument.
static E1000Core* g_activeCore = 0;

static void defaultReadMac(E1000Core* c) {
	// QEMU's 82540EM/82574L auto-load RAL0/RAH0 from the EEPROM at reset; I219 keeps them populated
	// by the firmware. The simplest reliable source on silicon and in QEMU.
	uint32_t ral = rd(c, REG_RAL0), rah = rd(c, REG_RAH0);
	c->mac[0] = ral & 0xff; c->mac[1] = (ral >> 8) & 0xff; c->mac[2] = (ral >> 16) & 0xff; c->mac[3] = (ral >> 24) & 0xff;
	c->mac[4] = rah & 0xff; c->mac[5] = (rah >> 8) & 0xff;
}

// Drain completed RX descriptors -> stack. NAPI: runs in the ISR/MSI handler (IF=0); no stack work.
void coreRxPoll(E1000Core* c) {
	while (rxDescDone(&c->rx[c->rxCur])) {
		uint16_t len = c->rx[c->rxCur].length;
		if (c->rx[c->rxCur].status & RXD_EOP)
			knx_netif_rx(c->handle, c->rxBuf[c->rxCur], len);   // copies into a NetBuf; we recycle now
		c->rx[c->rxCur].status = 0;
		wr(c, REG_RDT, c->rxCur);            // hand this descriptor back to the hardware
		c->rxCur = ringNext(c->rxCur, RX_N);
	}
}

// knx transmit callback body. Called from the softirq thread (the only tx context).
int coreTx(E1000Core* c, const void* data, int len) {
	if (len <= 0 || len > BUF_SZ)
		return -1;
	int i = c->txCur;
	memcpy(c->txBuf[i], data, len);
	txEncode(&c->tx[i], c->txBufPhys[i], len);
	c->txCur = ringNext(i, TX_N);
	wr(c, REG_TDT, c->txCur);
	// Bounded wait for the descriptor to be consumed so a wrap never overwrites an in-flight buffer.
	for (int t = 0; t < 2000000; t++)
		if (c->tx[i].status & TXD_DD)
			break;
	return 0;
}

static void coreIrq(void*) {
	E1000Core* c = g_activeCore;
	if (!c)
		return;
	uint32_t icr = rd(c, REG_ICR);          // reading ICR clears the asserted causes
	if (icr == 0)
		return;
	if (icr & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO))
		coreRxPoll(c);
	// TXDW: tx path polls DD itself; LSC: link change — nothing to do here.
}

static int coreTxTrampoline(void* ctx, const void* data, int len) {
	return coreTx((E1000Core*) ctx, data, len);
}

static bool setupRings(E1000Core* c) {
	uint32_t pa;
	c->rx = (RxDesc*) knx_dma_alloc(RX_N * sizeof(RxDesc), &c->rxPhys);
	c->tx = (TxDesc*) knx_dma_alloc(TX_N * sizeof(TxDesc), &c->txPhys);
	if (!c->rx || !c->tx)
		return false;
	for (int i = 0; i < RX_N; i++) {
		c->rxBuf[i] = (uint8_t*) knx_dma_alloc(BUF_SZ, &pa);
		if (!c->rxBuf[i]) return false;
		c->rx[i].addr = pa;
		c->rx[i].status = 0;
	}
	for (int i = 0; i < TX_N; i++) {
		c->txBuf[i] = (uint8_t*) knx_dma_alloc(BUF_SZ, &c->txBufPhys[i]);
		if (!c->txBuf[i]) return false;
		c->tx[i].status = TXD_DD;   // mark "done" so the first use sees a free descriptor
	}
	return true;
}

// Install the interrupt path: MSI-X -> MSI -> legacy INTx (Linux's order). Returns nothing; the
// caller has already set up the rings before causes are unmasked.
static void irqSetup(E1000Core* c) {
	bool wired = false;
	if (c->v->irqMode != IRQ_INTX)
		wired = (knx_register_msi(c->bus, c->dev, c->func, coreIrq, c) == 0);
	if (!wired)
		knx_register_irq(knx_pci_irq(c->bus, c->dev, c->func), coreIrq);   // legacy INTx fallback
}

int coreStart(E1000Core* c, const E1000Variant* v, uint8_t bus, uint8_t dev, uint8_t func) {
	c->v = v;
	c->bus = bus; c->dev = dev; c->func = func;
	c->rxCur = 0; c->txCur = 0; c->handle = 0;
	knx_pci_enable_bus_master(bus, dev, func);
	uint32_t bar0 = knx_pci_bar(bus, dev, func, 0);
	c->mmio = (volatile uint8_t*) knx_map_mmio(bar0, 0x20000);   // 128 KiB register window
	if (!c->mmio) { knx_log("e1000: BAR0 map failed\n"); return -1; }

	// Reset, then re-mask interrupts (reset re-enables some).
	wr(c, REG_IMC, 0xffffffff);
	wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_RST);
	for (volatile int d = 0; d < 100000; d++) { }
	for (int i = 0; i < 1000; i++) if (!(rd(c, REG_CTRL) & CTRL_RST)) break;
	wr(c, REG_IMC, 0xffffffff);

	// Variant PHY bring-up (I219 ich9lan ME/MDIC/ULP); abort cleanly if the link can't come up.
	if (v->phyBringup && !v->phyBringup(c)) { knx_log("e1000: phy bring-up failed\n"); return -1; }

	if (v->readMac) { if (!v->readMac(c)) { knx_log("e1000: MAC read failed\n"); return -1; } }
	else defaultReadMac(c);

	wr(c, REG_CTRL, rd(c, REG_CTRL) | CTRL_SLU | CTRL_ASDE);   // link up, auto-speed detect
	for (int i = 0; i < 128; i++) wr(c, REG_MTA + i * 4, 0);    // clear multicast table

	wr(c, REG_RAL0, (uint32_t) c->mac[0] | ((uint32_t) c->mac[1] << 8) | ((uint32_t) c->mac[2] << 16) | ((uint32_t) c->mac[3] << 24));
	wr(c, REG_RAH0, (uint32_t) c->mac[4] | ((uint32_t) c->mac[5] << 8) | RAH_AV);

	if (!setupRings(c)) { knx_log("e1000: DMA ring alloc failed\n"); return -1; }

	wr(c, REG_RDBAL, c->rxPhys); wr(c, REG_RDBAH, 0);
	wr(c, REG_RDLEN, RX_N * sizeof(RxDesc));
	wr(c, REG_RDH, 0); wr(c, REG_RDT, RX_N - 1);
	wr(c, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

	if (v->rxCsumOffload)
		wr(c, REG_RXCSUM, RXCSUM_IPOFL | RXCSUM_TUOFL);   // hardware verifies RX IP/TCP/UDP checksums

	wr(c, REG_TDBAL, c->txPhys); wr(c, REG_TDBAH, 0);
	wr(c, REG_TDLEN, TX_N * sizeof(TxDesc));
	wr(c, REG_TDH, 0); wr(c, REG_TDT, 0);
	wr(c, REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));   // CT=15, COLD=64 (full duplex)
	wr(c, REG_TIPG, 0x0060200A);                                        // copper inter-packet gap

	g_activeCore = c;
	irqSetup(c);
	wr(c, REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO | ICR_LSC);   // unmask the causes we handle
	rd(c, REG_ICR);                                              // clear any latched causes

	KnxNetDev nd;
	for (int i = 0; i < 16; i++) nd.name[i] = 0;
	nd.name[0] = 'e'; nd.name[1] = 't'; nd.name[2] = 'h'; nd.name[3] = '0';
	for (int i = 0; i < 6; i++) nd.mac[i] = c->mac[i];
	nd.mtu = 1500; nd.tx = coreTxTrampoline; nd.drvctx = c;
	c->handle = knx_add_net_dev(&nd);
	if (!c->handle) { knx_log("e1000: add_net_dev failed\n"); return -1; }

	knx_log(v->tag); knx_log(": eth0 up\n");
	return 0;
}

#endif  // NANOS_HOST_TEST
