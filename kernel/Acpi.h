#pragma once
#include <stdint.h>

namespace kernel {

// Pure ACPI walker: RSDP -> RSDT/XSDT -> MADT (APIC) -> enabled LAPIC ids. Reads physical
// memory through an injected callback so it host-tests against a crafted blob and, in the
// kernel, reads through the identity map. No allocation; a fixed cap on CPUs.
class Acpi {
public:
	typedef bool (*ReadFn)(uint64_t pa, void* dst, uint32_t len);
	static const int MAX_CPUS = 32;

	explicit Acpi(ReadFn r) : read(r), nCpus(0), lapicPhys(0) {}
	bool parse(uint64_t rsdpPhys);     // false if RSDP/checksums/MADT invalid
	int  cpuCount() const { return nCpus; }
	uint8_t lapicId(int i) const { return ids[i]; }
	uint64_t lapicAddr() const { return lapicPhys; }   // LAPIC MMIO base from the MADT

private:
	ReadFn read;
	int nCpus;
	uint8_t ids[MAX_CPUS];
	uint64_t lapicPhys;
	bool checksumOk(uint64_t pa, uint32_t len);
	bool parseMadt(uint64_t pa);
};

}  // namespace kernel
