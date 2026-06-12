/*
 * arch/pci.h — MI/MD contract for PCI configuration-space access.
 *
 * The mechanism is arch-specific (x86 PC: the 0xCF8 address / 0xCFC data port pair,
 * "configuration mechanism #1"; another arch: an ECAM MMIO window). MI code
 * (kernel/Pci) enumerates the bus and decodes BARs purely through these two 32-bit
 * accessors — it never touches a port. The kernel installs these as the backend of
 * kernel::Pci at boot (kernel::Pci::setBackend), so the MI core stays host-testable
 * against a mock config space.
 *
 * Config space is naturally dword-addressed; 8/16-bit reads/writes are derived in the
 * MI core by masking/RMW, so the MD surface is just these two functions.
 */
#pragma once
#include <stdint.h>

namespace arch {

uint32_t pciConfigRead32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);

}  // namespace arch
