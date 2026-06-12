/*
 * e1000.cpp — the Intel 82540EM (PCI 8086:100E) gigabit NIC as a loadable module (e1000.nkext).
 * The canonical e1000 programming model: MMIO BAR0 registers, EEPROM-loaded MAC, legacy 16-byte
 * RX/TX descriptor rings in DMA memory, legacy IRQ. This is the only large machine-dependent
 * piece of the network stack; everything above net device is MI.
 *
 *   RX: hardware DMAs frames into our ring buffers, raises RXT0 -> our IRQ handler copies each
 *       completed frame to the stack via knx_netif_rx (which enqueues to the softirq thread) and
 *       hands the descriptor back (RDT). NO stack work in IRQ.
 *   TX: knx tx callback copies the frame into the next ring buffer, bumps TDT, waits for DD.
 *
 * The kext does its own volatile MMIO (it is ring-0 MD code, outside the MI arch guard).
 */
#include "knx_net.h"   // struct KnxNetDev + knx_map_mmio/dma_alloc/add_net_dev/netif_rx
#include <stdint.h>
#include <stddef.h>

// Kernel exports this module imports (resolved against KernelExports at load).
extern "C" int  knx_pci_find(uint16_t vendor, uint16_t device, uint8_t* bus, uint8_t* dev, uint8_t* func);
extern "C" uint32_t knx_pci_bar(uint8_t bus, uint8_t dev, uint8_t func, int n);
extern "C" uint8_t  knx_pci_irq(uint8_t bus, uint8_t dev, uint8_t func);
extern "C" void knx_pci_enable_bus_master(uint8_t bus, uint8_t dev, uint8_t func);
extern "C" void knx_register_irq(int irq, void (*h)(void*));
extern "C" void knx_log(const char* s);
extern "C" void* memcpy(void*, const void*, size_t);

namespace {

// ---- register offsets (bytes from BAR0) ----
enum {
	REG_CTRL = 0x0000, REG_STATUS = 0x0008, REG_EERD = 0x0014,
	REG_ICR = 0x00C0, REG_ICS = 0x00C8, REG_IMS = 0x00D0, REG_IMC = 0x00D8,
	REG_RCTL = 0x0100, REG_TCTL = 0x0400, REG_TIPG = 0x0410,
	REG_RDBAL = 0x2800, REG_RDBAH = 0x2804, REG_RDLEN = 0x2808, REG_RDH = 0x2810, REG_RDT = 0x2818,
	REG_TDBAL = 0x3800, REG_TDBAH = 0x3804, REG_TDLEN = 0x3808, REG_TDH = 0x3810, REG_TDT = 0x3818,
	REG_MTA = 0x5200, REG_RAL0 = 0x5400, REG_RAH0 = 0x5404,
};
enum {
	CTRL_SLU = 0x00000040, CTRL_ASDE = 0x00000020, CTRL_RST = 0x04000000,
	RCTL_EN = 0x00000002, RCTL_BAM = 0x00008000, RCTL_SECRC = 0x04000000, RCTL_BSIZE_2048 = 0,
	TCTL_EN = 0x00000002, TCTL_PSP = 0x00000008,
	ICR_TXDW = 0x01, ICR_LSC = 0x04, ICR_RXDMT0 = 0x10, ICR_RXO = 0x40, ICR_RXT0 = 0x80,
	RXD_DD = 0x01, RXD_EOP = 0x02,
	TXD_EOP = 0x01, TXD_IFCS = 0x02, TXD_RS = 0x08, TXD_DD = 0x01,
	RAH_AV = 0x80000000,
};

struct RxDesc { uint64_t addr; uint16_t length; uint16_t csum; uint8_t status; uint8_t errors; uint16_t special; } __attribute__((packed));
struct TxDesc { uint64_t addr; uint16_t length; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; } __attribute__((packed));

const int RX_N = 32;          // 32 * 16 = 512 B ring (fits one 4 KiB DMA frame)
const int TX_N = 32;
const int BUF_SZ = 2048;

volatile uint8_t* g_mmio = 0;
RxDesc* g_rx = 0; uint32_t g_rxPhys = 0;
TxDesc* g_tx = 0; uint32_t g_txPhys = 0;
uint8_t*  g_rxBuf[RX_N];
uint8_t*  g_txBuf[TX_N]; uint32_t g_txBufPhys[TX_N];
int g_rxCur = 0, g_txCur = 0;
void* g_handle = 0;
unsigned char g_mac[6];

inline void wr(int off, uint32_t v) { *(volatile uint32_t*) (g_mmio + off) = v; }
inline uint32_t rd(int off) { return *(volatile uint32_t*) (g_mmio + off); }

void readMac() {
	// QEMU's 82540EM auto-loads the receive-address registers from the EEPROM at reset, so RAL0/
	// RAH0 hold the MAC — the simplest reliable source on real silicon and in QEMU.
	uint32_t ral = rd(REG_RAL0), rah = rd(REG_RAH0);
	g_mac[0] = ral & 0xff; g_mac[1] = (ral >> 8) & 0xff; g_mac[2] = (ral >> 16) & 0xff; g_mac[3] = (ral >> 24) & 0xff;
	g_mac[4] = rah & 0xff; g_mac[5] = (rah >> 8) & 0xff;
}

// Drain completed RX descriptors -> stack. Called only from the IRQ handler (IF=0).
void rxPoll() {
	while (g_rx[g_rxCur].status & RXD_DD) {
		uint16_t len = g_rx[g_rxCur].length;
		if (g_rx[g_rxCur].status & RXD_EOP)
			knx_netif_rx(g_handle, g_rxBuf[g_rxCur], len);   // copies into a NetBuf; we recycle now
		g_rx[g_rxCur].status = 0;
		wr(REG_RDT, g_rxCur);            // give this descriptor back to the hardware
		g_rxCur = (g_rxCur + 1) % RX_N;
	}
}

void e1000Irq(void*) {
	uint32_t icr = rd(REG_ICR);          // reading ICR clears the asserted causes
	if (icr == 0)
		return;
	if (icr & (ICR_RXT0 | ICR_RXDMT0 | ICR_RXO))
		rxPoll();
	// TXDW: we poll DD in the tx path; LSC: link change — nothing to do for our purposes.
}

// knx transmit callback: copy the frame into the next TX ring buffer and kick the hardware.
// Called from the softirq thread (the only tx context), so g_txCur needs no lock vs the IRQ
// (which only touches g_rxCur).
int e1000Tx(void*, const void* data, int len) {
	if (len <= 0 || len > BUF_SZ)
		return -1;
	int i = g_txCur;
	memcpy(g_txBuf[i], data, len);
	g_tx[i].addr = g_txBufPhys[i];
	g_tx[i].length = (uint16_t) len;
	g_tx[i].cso = 0;
	g_tx[i].cmd = TXD_EOP | TXD_IFCS | TXD_RS;   // end-of-packet, insert FCS, report status
	g_tx[i].status = 0;
	g_txCur = (i + 1) % TX_N;
	wr(REG_TDT, g_txCur);
	// Wait (bounded) for the descriptor to be consumed so we never overwrite an in-flight buffer
	// on wrap. QEMU completes immediately; the bound prevents a hang on a wedged device.
	for (int t = 0; t < 2000000; t++)
		if (g_tx[i].status & TXD_DD)
			break;
	return 0;
}

bool setupRings() {
	uint32_t pa;
	g_rx = (RxDesc*) knx_dma_alloc(RX_N * sizeof(RxDesc), &g_rxPhys);
	g_tx = (TxDesc*) knx_dma_alloc(TX_N * sizeof(TxDesc), &g_txPhys);
	if (!g_rx || !g_tx)
		return false;
	for (int i = 0; i < RX_N; i++) {
		g_rxBuf[i] = (uint8_t*) knx_dma_alloc(BUF_SZ, &pa);
		if (!g_rxBuf[i]) return false;
		g_rx[i].addr = pa;
		g_rx[i].status = 0;
	}
	for (int i = 0; i < TX_N; i++) {
		g_txBuf[i] = (uint8_t*) knx_dma_alloc(BUF_SZ, &g_txBufPhys[i]);
		if (!g_txBuf[i]) return false;
		g_tx[i].status = TXD_DD;   // mark "done" so the first use sees a free descriptor
	}
	return true;
}

}  // namespace

extern "C" int nkext_init() {
	uint8_t bus, dev, func;
	if (!knx_pci_find(0x8086, 0x100E, &bus, &dev, &func)) {
		knx_log("e1000: no 8086:100E found\n");
		return -1;
	}
	knx_pci_enable_bus_master(bus, dev, func);
	uint32_t bar0 = knx_pci_bar(bus, dev, func, 0);
	g_mmio = (volatile uint8_t*) knx_map_mmio(bar0, 0x20000);   // 128 KiB register window
	if (!g_mmio) { knx_log("e1000: BAR0 map failed\n"); return -1; }

	// Reset the device, then re-mask interrupts (reset re-enables some).
	wr(REG_IMC, 0xffffffff);
	wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
	for (volatile int d = 0; d < 100000; d++) { }
	for (int i = 0; i < 1000; i++) if (!(rd(REG_CTRL) & CTRL_RST)) break;
	wr(REG_IMC, 0xffffffff);

	readMac();
	wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);   // set link up, auto-speed detect

	for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);  // clear multicast table

	// Program our receive address (QEMU auto-loaded it, but assert AV explicitly).
	wr(REG_RAL0, (uint32_t) g_mac[0] | ((uint32_t) g_mac[1] << 8) | ((uint32_t) g_mac[2] << 16) | ((uint32_t) g_mac[3] << 24));
	wr(REG_RAH0, (uint32_t) g_mac[4] | ((uint32_t) g_mac[5] << 8) | RAH_AV);

	if (!setupRings()) { knx_log("e1000: DMA ring alloc failed\n"); return -1; }

	wr(REG_RDBAL, g_rxPhys); wr(REG_RDBAH, 0);
	wr(REG_RDLEN, RX_N * sizeof(RxDesc));
	wr(REG_RDH, 0); wr(REG_RDT, RX_N - 1);
	wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

	wr(REG_TDBAL, g_txPhys); wr(REG_TDBAH, 0);
	wr(REG_TDLEN, TX_N * sizeof(TxDesc));
	wr(REG_TDH, 0); wr(REG_TDT, 0);
	wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));   // CT=15, COLD=64 (full duplex)
	wr(REG_TIPG, 0x0060200A);                                        // copper inter-packet gap

	// Install the IRQ handler, then unmask the causes we care about.
	knx_register_irq(knx_pci_irq(bus, dev, func), e1000Irq);
	wr(REG_IMS, ICR_RXT0 | ICR_RXDMT0 | ICR_RXO | ICR_LSC);
	rd(REG_ICR);   // clear any latched causes

	// Publish eth0 to the stack.
	KnxNetDev d;
	for (int i = 0; i < 16; i++) d.name[i] = 0;
	d.name[0] = 'e'; d.name[1] = 't'; d.name[2] = 'h'; d.name[3] = '0';
	for (int i = 0; i < 6; i++) d.mac[i] = g_mac[i];
	d.mtu = 1500; d.tx = e1000Tx; d.drvctx = 0;
	g_handle = knx_add_net_dev(&d);
	if (!g_handle) { knx_log("e1000: add_net_dev failed\n"); return -1; }

	knx_log("e1000: eth0 up\n");
	return 0;
}
