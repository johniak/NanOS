/*
 * i219.cpp — Intel I219-LM (PCI 8086:0D4E, Comet Lake PCH) as i219.nkext — the Dell Latitude 5310 NIC.
 *
 * A thin variant over E1000Core plus the ich9lan PHY bring-up (i219_phy.cpp). QEMU has no I219 model,
 * so this binds only on real hardware; on QEMU knx_pci_find fails and nkext_init returns -1 quietly
 * (zero effect on the e1000/e1000e kexts).
 */
#include "e1000_core.h"
#include "i219_phy.h"

extern "C" int  knx_pci_find(uint16_t vendor, uint16_t device, uint8_t* bus, uint8_t* dev, uint8_t* func);
extern "C" void knx_log(const char* s);

static E1000Core g_core;
static const E1000Variant g_i219 = {
	0x0D4E, "i219", /*phyBringup*/ i219PhyBringup, /*readMac*/ 0, IRQ_MSI, /*rxCsumOffload*/ true,
};

extern "C" int nkext_init() {
	uint8_t bus, dev, func;
	if (!knx_pci_find(0x8086, 0x0D4E, &bus, &dev, &func)) {
		knx_log("i219: no 8086:0D4E found\n");
		return -1;
	}
	return coreStart(&g_core, &g_i219, bus, dev, func);
}
