/*
 * PagingControl.h
 *
 * Thin inline-asm wrappers over the paging control registers (CR3/CR0/CR2).
 * Hardware-only — not host-testable, excluded from the coverage gate. AT&T
 * inline asm, matching arch/IOPort.cpp.
 */
#pragma once
#include <stdint.h>

namespace kernel {

// Load the page-directory physical address into CR3.
inline void loadCr3(uint32_t pdPhys) {
	__asm__ __volatile__("mov %0, %%cr3" : : "r"(pdPhys) : "memory");
}

// Set CR0.PG (bit 31) to turn paging on. The next instruction must execute at a
// mapped (identity) address — satisfied because the kernel .text is identity-mapped.
inline void enablePaging() {
	uint32_t cr0;
	__asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x80000000u;
	__asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

// Faulting linear address after a page fault (for the future #PF handler).
inline uint32_t readCr2() {
	uint32_t v;
	__asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
	return v;
}

}
