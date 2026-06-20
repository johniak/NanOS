/*
 * e1000e.cpp — the Intel 82574L (PCI 8086:10D3) gigabit NIC as a loadable module (e1000e.nkext).
 *
 * A thin wrapper over the shared E1000Core: the 82574L is a discrete PCIe e1000e part (no ich9lan
 * PHY/ME dance), so it needs only MSI-X + RX checksum offload over the canonical register model.
 * QEMU emulates `-device e1000e` as the 82574L, so this kext is the headless test vehicle for the
 * whole shared core + MSI-X + NAPI path that the (QEMU-absent) I219 also relies on.
 */
#include "e1000_core.h"

extern "C" int  knx_pci_find(uint16_t vendor, uint16_t device, uint8_t* bus, uint8_t* dev, uint8_t* func);
extern "C" void knx_log(const char* s);

static E1000Core g_core;
static const E1000Variant g_e1000e = {
	0x10D3, "e1000e", /*phyBringup*/ 0, /*readMac*/ 0, IRQ_MSI, /*rxCsumOffload*/ true,
};

extern "C" int nkext_init() {
	uint8_t bus, dev, func;
	if (!knx_pci_find(0x8086, 0x10D3, &bus, &dev, &func)) {
		knx_log("e1000e: no 8086:10D3 found\n");
		return -1;
	}
	return coreStart(&g_core, &g_e1000e, bus, dev, func);
}
