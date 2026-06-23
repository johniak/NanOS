#pragma once
#include <stdint.h>

// x86_64 ACPI glue (MD): locate the firmware RSDP in low memory and run the MI kernel::Acpi
// parser over the identity-mapped ACPI tables to enumerate the CPUs' local-APIC ids.
namespace arch {

uint64_t acpiFindRsdp();   // physical address of the RSDP, or 0 if not found

// Enumerate enabled CPUs. Fills idsOut[0..n) with local-APIC ids (n capped at maxOut),
// optionally returns the LAPIC MMIO base, and returns the CPU count (0 if no ACPI/MADT).
int acpiEnumCpus(uint8_t* idsOut, int maxOut, uint64_t* lapicAddrOut);

}  // namespace arch
