/*
 * e1000.cpp — the Intel 82540EM (PCI 8086:100E) NIC as a loadable module (e1000.nkext).
 *
 * A thin wrapper over the shared E1000Core engine: match the device, then coreStart(). The 82540EM
 * is the QEMU default NIC; it uses legacy INTx and no checksum offload, so its programming through
 * the core is byte-for-byte what it always was (the behaviour-preservation gate is `make smoke-x86_64`).
 */
#include "e1000_core.h"

extern "C" int  knx_pci_find(uint16_t vendor, uint16_t device, uint8_t* bus, uint8_t* dev, uint8_t* func);
extern "C" void knx_log(const char* s);

static E1000Core g_core;
static const E1000Variant g_e1000 = {
	0x100E, "e1000", /*phyBringup*/ 0, /*readMac*/ 0, IRQ_INTX, /*rxCsumOffload*/ false,
};

extern "C" int nkext_init() {
	uint8_t bus, dev, func;
	if (!knx_pci_find(0x8086, 0x100E, &bus, &dev, &func)) {
		knx_log("e1000: no 8086:100E found\n");
		return -1;
	}
	return coreStart(&g_core, &g_e1000, bus, dev, func);
}
