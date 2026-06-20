/*
 * e1000_core.h — the shared Intel "e1000" engine behind the per-variant NIC kexts.
 *
 * The canonical e1000 programming model (MMIO BAR0 registers, legacy 16-byte RX/TX descriptor rings
 * in DMA memory) is identical across the 82540EM (e1000), 82574L (e1000e) and I219 (ich9lan). This
 * engine holds all of it in one place; each kext is a thin wrapper that fills an E1000Variant and
 * calls coreStart(). Variant differences live behind hooks (PHY bring-up, MAC read) and flags
 * (interrupt mode, RX checksum offload).
 *
 * The pure-logic helpers (ringNext / rxDescDone / txEncode) are compiled everywhere and host-tested
 * (tests/test_e1000_core.cpp); the engine half (coreStart/coreRxPoll/coreTx) needs the knx_* kernel
 * imports and so is guarded out of the host build with NANOS_HOST_TEST.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum E1000IrqMode { IRQ_INTX = 0, IRQ_MSI = 1, IRQ_MSIX = 2 };

struct E1000Core;

struct E1000Variant {
	uint16_t    pciDevice;              // 0x100E (e1000) / 0x10D3 (e1000e) / 0x0D4E (I219)
	const char* tag;                    // "e1000" / "e1000e" / "i219" — for logging
	bool        (*phyBringup)(E1000Core*);   // I219 ich9lan PHY/ME/ULP; null = none (e1000/e1000e)
	bool        (*readMac)(E1000Core*);      // null = default (read RAL0/RAH0)
	int         irqMode;                // preferred E1000IrqMode; degrades MSI-X -> MSI -> INTX
	bool        rxCsumOffload;          // program RXCSUM (hardware verifies RX IP/TCP/UDP checksums)
};

struct RxDesc { uint64_t addr; uint16_t length; uint16_t csum; uint8_t status; uint8_t errors; uint16_t special; } __attribute__((packed));
struct TxDesc { uint64_t addr; uint16_t length; uint8_t cso; uint8_t cmd; uint8_t status; uint8_t css; uint16_t special; } __attribute__((packed));

static const int RX_N = 32;            // 32 * 16 = 512 B ring (fits one 4 KiB DMA frame)
static const int TX_N = 32;
static const int BUF_SZ = 2048;

struct E1000Core {
	volatile uint8_t*  mmio;
	const E1000Variant* v;
	uint8_t            bus, dev, func;
	RxDesc*            rx;  uint32_t rxPhys;
	TxDesc*            tx;  uint32_t txPhys;
	uint8_t*           rxBuf[RX_N];
	uint8_t*           txBuf[TX_N];  uint32_t txBufPhys[TX_N];
	int                rxCur, txCur;
	void*              handle;        // NetDevice handle from knx_add_net_dev
	unsigned char      mac[6];
};

// ---- pure-logic helpers (host-tested; no MMIO / no knx) ----
int  ringNext(int cur, int n);                       // (cur + 1) % n
bool rxDescDone(const RxDesc* d);                     // status & DD
void txEncode(TxDesc* d, uint32_t bufPhys, int len);  // fill addr/length/cmd(EOP|IFCS|RS), clear status

// ---- engine entry points (kernel/kext side; need the knx_* imports) ----
int  coreStart(E1000Core* c, const E1000Variant* v, uint8_t bus, uint8_t dev, uint8_t func);
void coreRxPoll(E1000Core* c);                        // NAPI drain (from the ISR/MSI handler)
int  coreTx(E1000Core* c, const void* data, int len); // the knx_tx_fn body
