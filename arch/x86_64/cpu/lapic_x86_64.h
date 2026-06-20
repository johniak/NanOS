/*
 * lapic_x86_64.h — minimal Local APIC bring-up + an MSI interrupt-vector allocator.
 *
 * The LAPIC runs ALONGSIDE the existing 8259 PIC: the PIC keeps delivering the legacy ISA IRQs
 * (timer/keyboard/ATA on vectors 0x20..0x2F), and the LAPIC only adds MSI/MSI-X delivery for PCIe
 * devices (the NIC). No IOAPIC — MSI bypasses it (the message goes straight to the LAPIC).
 *
 * The vector-pool allocator is split out as pure logic (struct MsiVecPool + msiVec*) so it is
 * host-testable independent of the LAPIC MMIO/MSR, which only builds in the kernel.
 */
#pragma once
#include <stdint.h>

namespace kernel {

// MSI vectors live above the PIC IRQ range (0x20..0x2F). 8 vectors, matching the 8 IDT stubs the
// kernel installs (irq_msi0..7 -> 0x70..0x77).
constexpr uint8_t MSI_VEC_BASE = 0x70;
constexpr uint8_t MSI_VEC_END  = 0x78;   // exclusive

struct MsiVecPool { uint8_t next; };
void msiVecPoolInit(MsiVecPool* p);      // next = MSI_VEC_BASE
int  msiVecAlloc(MsiVecPool* p);         // a vector in [BASE,END) or -1 if exhausted

// LAPIC bring-up + accessors (kernel-only; MMIO at the IA32_APIC_BASE physical, identity-mapped).
void    lapicInit();                     // enable LAPIC + spurious vector; safe to call once
uint8_t lapicId();                       // local APIC id (for the MSI message address)
void    lapicEoi();                      // signal end-of-interrupt
int     lapicAllocVector();              // global pool alloc (wraps msiVecAlloc); -1 if full

}  // namespace kernel
