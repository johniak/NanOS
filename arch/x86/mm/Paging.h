/*
 * Paging.h
 *
 * x86 32-bit paging bit layout and address math. Pure inline helpers shared by
 * AddressSpace (and later the page-fault handler). 4 KiB pages, two-level
 * directory/table walk: va = [ 10-bit PD index | 10-bit PT index | 12-bit off ].
 */
#pragma once
#include <stdint.h>

namespace kernel {

const uint32_t PTE_PRESENT = 0x1;
const uint32_t PTE_RW = 0x2;
const uint32_t PTE_USER = 0x4;
const uint32_t PTE_PWT = 0x8;    // Page Write-Through
const uint32_t PTE_PCD = 0x10;   // Page Cache Disable — PCD+PWT = UC, required for device MMIO
const uint32_t PAGE_MASK = 0xFFFFF000;

inline uint32_t pdIndex(uint32_t va) { return (va >> 22) & 0x3FF; }
inline uint32_t ptIndex(uint32_t va) { return (va >> 12) & 0x3FF; }
inline uint32_t pageOffset(uint32_t va) { return va & 0xFFF; }

inline uint32_t makeEntry(uint32_t pa, uint32_t flags) {
	return (pa & PAGE_MASK) | (flags & 0xFFF);
}
inline uint32_t entryAddr(uint32_t e) { return e & PAGE_MASK; }
inline bool entryPresent(uint32_t e) { return e & PTE_PRESENT; }

}
