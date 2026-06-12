/*
 * pci_x86.cpp — x86 PCI configuration-space access (<arch/pci.h>), "configuration
 * mechanism #1": write a dword address to CONFIG_ADDRESS (0xCF8), read/write the dword
 * at CONFIG_DATA (0xCFC).
 *
 * CONFIG_ADDRESS layout (Intel):
 *   bit 31    = enable
 *   bits30:24 = reserved (0)
 *   bits23:16 = bus
 *   bits15:11 = device (0..31)
 *   bits10:8  = function (0..7)
 *   bits7:2   = register (dword index)   ← offset must be dword-aligned
 *   bits1:0   = 0
 *
 * This is the only machine-dependent PCI code; enumeration/BAR decode are MI (kernel/Pci).
 */
#include <arch/pci.h>

namespace {

inline void outl(unsigned short port, uint32_t v) {
	__asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}
inline uint32_t inl(unsigned short port) {
	uint32_t v;
	__asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

constexpr unsigned short CONFIG_ADDRESS = 0xCF8;
constexpr unsigned short CONFIG_DATA    = 0xCFC;

inline uint32_t address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
	return (uint32_t) 0x80000000u
	     | ((uint32_t) bus << 16)
	     | ((uint32_t) (dev & 0x1f) << 11)
	     | ((uint32_t) (func & 0x07) << 8)
	     | ((uint32_t) (off & 0xfc));   // dword-aligned register
}

}  // namespace

namespace arch {

uint32_t pciConfigRead32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
	outl(CONFIG_ADDRESS, address(bus, dev, func, off));
	return inl(CONFIG_DATA);
}

void pciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val) {
	outl(CONFIG_ADDRESS, address(bus, dev, func, off));
	outl(CONFIG_DATA, val);
}

}  // namespace arch
