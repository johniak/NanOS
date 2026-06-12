/*
 * Pci.h — machine-independent PCI bus core: enumeration + BAR decode over an injectable
 * config-space backend. The backend is two 32-bit accessors (arch ports in the kernel via
 * <arch/pci.h>; a mock table in host tests), so this whole module is host-testable with no
 * hardware. This is FAZA 1 of the networking plan — the foundation that lets us find the NIC.
 */
#pragma once
#include <stdint.h>

namespace kernel {

// One decoded Base Address Register. addr/size are 0 for an unused slot.
struct PciBar {
	uint32_t addr;      // base address (low flag bits masked off); for I/O BARs, the port base
	uint32_t size;      // region size in bytes (power of two), 0 = unused
	bool     isIo;      // true = I/O-space BAR, false = memory-mapped BAR
	bool     is64;      // 64-bit memory BAR (its high dword lives in the next slot, which is then 0)
	bool     prefetch;  // prefetchable memory
};

struct PciDevice {
	uint8_t  bus, dev, func;
	uint16_t vendor, device;
	uint8_t  classCode, subclass, progIf, revision;
	uint8_t  headerType;            // low 7 bits = layout; bit7 = multifunction
	uint8_t  irqLine, irqPin;
	PciBar   bar[6];
};

typedef uint32_t (*PciCfgRead32)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
typedef void     (*PciCfgWrite32)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);

class Pci {
public:
	// Install the config-space backend (arch::pciConfigRead32/Write32 in the kernel; a mock in
	// tests). Must be called before any other method.
	static void setBackend(PciCfgRead32 rd, PciCfgWrite32 wr);
	static bool hasBackend();

	// Raw config accessors. 8/16-bit derive from the 32-bit backend (mask / read-modify-write).
	static uint32_t read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
	static uint16_t read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
	static uint8_t  read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
	static void     write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);
	static void     write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t v);
	static void     write8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint8_t v);

	// Probe one slot: fills out and returns true iff present (vendor != 0xFFFF).
	static bool probe(uint8_t bus, uint8_t dev, uint8_t func, PciDevice& out);

	// Brute-force scan of the whole bus space into out[] (up to max). Returns the count found.
	static int  enumerate(PciDevice* out, int max);

	// First device matching vendor:device (0xFFFF = wildcard). Returns true + fills out.
	static bool find(uint16_t vendor, uint16_t device, PciDevice& out);

	// Command-register helpers (RMW). Memory NICs need MASTER + MEM; legacy ones may need IO.
	static void enableBusMaster(const PciDevice& d);
	static void enableMemSpace(const PciDevice& d);
	static void enableIoSpace(const PciDevice& d);

private:
	static void readBars(PciDevice& d);
};

// Standard config-space register offsets (type-0 header).
enum {
	PCI_VENDOR_ID   = 0x00, PCI_DEVICE_ID = 0x02, PCI_COMMAND = 0x04, PCI_STATUS  = 0x06,
	PCI_REVISION    = 0x08, PCI_PROG_IF   = 0x09, PCI_SUBCLASS = 0x0a, PCI_CLASS  = 0x0b,
	PCI_HEADER_TYPE = 0x0e, PCI_BAR0      = 0x10, PCI_IRQ_LINE = 0x3c, PCI_IRQ_PIN = 0x3d,
};
enum { PCI_CMD_IO = 0x1, PCI_CMD_MEM = 0x2, PCI_CMD_MASTER = 0x4 };
enum { PCI_VENDOR_NONE = 0xFFFF };

}  // namespace kernel
