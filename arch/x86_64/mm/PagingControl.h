/*
 * PagingControl.h
 *
 * Thin inline-asm wrappers over the paging control state on x86_64: CR3 (PML4 phys),
 * CR2 (fault address), CR0.PG, and the EFER.NXE enable for the NX bit. Hardware-only —
 * not host-testable, excluded from the coverage gate. AT&T inline asm. On x86_64 paging
 * (CR0.PG) and PAE+LME are already on from the Plan-1 boot trampoline; loadCr3 here just
 * switches the active PML4.
 */
#pragma once
#include <stdint.h>

namespace kernel {

// Load the PML4 physical address into CR3.
inline void loadCr3(uint64_t pml4Phys) {
	__asm__ __volatile__("mov %0, %%cr3" : : "r"(pml4Phys) : "memory");
}

// Read the current PML4 physical address from CR3.
inline uint64_t readCr3() {
	uint64_t v;
	__asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
	return v;
}

// Faulting linear address after a page fault (for the future #PF handler).
inline uint64_t readCr2() {
	uint64_t v;
	__asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
	return v;
}

// Provided for symmetry with i686. Paging is already on in long mode; this is a no-op
// guard that re-asserts CR0.PG.
inline void enablePaging() {
	uint64_t cr0;
	__asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= (1ULL << 31);                                  // CR0.PG
	__asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

// Enable EFER.NXE (IA32_EFER MSR 0xC0000080, bit 11) so the NX bit (PTE bit 63) is
// honored instead of raising #GP on a reserved-bit set. Call once at kernel bring-up
// before installing any NX mapping.
inline void enableNxe() {
	uint32_t lo, hi;
	__asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
	lo |= (1u << 11);                                     // NXE
	__asm__ __volatile__("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080u));
}

}
