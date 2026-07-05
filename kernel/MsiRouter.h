/*
 * MsiRouter.h — MI logic for setting up an MSI / MSI-X interrupt on a PCI function.
 *
 * Walks the PCI capability list, prefers MSI-X (cap 0x11) over MSI (cap 0x05), allocates a CPU
 * interrupt vector, and programs the chosen capability (message address = the LAPIC, data = the
 * vector). All hardware touchpoints — config-space r/w, MMIO map (for the MSI-X table), the vector
 * allocator and the LAPIC id — are injected via MsiEnv, so the walk + programming is host-testable
 * with a fake config space and no hardware. The kernel wires the real backends in KernelExports.cpp.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct MsiEnv {
	uint32_t (*cfgRead)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
	void     (*cfgWrite)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v);
	void*    (*mapMmio)(uint64_t phys, uint64_t len);   // for the MSI-X table BAR; unused for MSI
	int      (*allocVector)();                          // a CPU vector (>=0) or -1 if none free
	uint8_t  (*lapicId)();                              // for the MSI message address
};

enum MsiKind { MSI_NONE = 0, MSI_KIND_MSI = 1, MSI_KIND_MSIX = 2 };

struct MsiResult { MsiKind kind; int vector; uint8_t capOff; };

// Walk caps at (bus,dev,func); prefer MSI-X over MSI. On a match: allocVector(), program the cap
// (addr = 0xFEE00000 | (lapicId << 12), data = vector) and enable it. Returns {kind,vector,capOff}
// or {MSI_NONE,-1,0} if neither cap exists / no vector is free.
MsiResult msiSetup(const MsiEnv& env, uint8_t bus, uint8_t dev, uint8_t func);

}  // namespace kernel
