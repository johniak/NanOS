/*
 * Paging.h
 *
 * x86_64 4-level paging bit layout and address math. Pure inline helpers shared by
 * AddressSpace (and later the #PF handler). 4 KiB pages, four-level walk:
 *   va = [ PML4(9) | PDPT(9) | PD(9) | PT(9) | off(12) ]  (512 entries per level)
 * Entries are 8 bytes; the frame address lives in bits 12..51 (40-bit physical), the
 * low 12 bits hold P/RW/US/PWT/PCD/A/D/PS/G, and bit 63 is NX (No-Execute).
 * Virtual addresses must be canonical: bits 48..63 sign-extend bit 47.
 */
#pragma once
#include <stdint.h>

namespace kernel {

const uint64_t PTE_PRESENT = 0x1;
const uint64_t PTE_RW      = 0x2;
const uint64_t PTE_USER    = 0x4;
const uint64_t PTE_PWT     = 0x8;                        // Page Write-Through (bit 3)
const uint64_t PTE_PCD     = 0x10;                       // Page Cache Disable (bit 4). PCD+PWT with the
                                                         // default PAT = strong UC — required for device
                                                         // MMIO (GPU registers): a cached register write
                                                         // buffers in the CPU cache and never reaches the
                                                         // device (forcewake never acks -> hang on real HW).
const uint64_t PTE_NX      = 1ULL << 63;                 // No-Execute (honored when EFER.NXE=1)
const uint64_t PTE_PS      = 1ULL << 7;                  // Page Size: a PD entry with this maps a 2 MiB page
const uint64_t PTE_PRIV    = 1ULL << 9;                  // AVL bit 9 (ignored by HW): this entry points
                                                         // at a PER-PROCESS private table — re-privatize
                                                         // is a no-op, and teardown frees it. Cleared on
                                                         // the shared kernel-half entries (adoptKernelDirectory).
const uint64_t PTE_SHARED  = 1ULL << 10;                 // AVL bit 10 (ignored by HW): this LEAF frame is
                                                         // owned by a shared object (an Shm behind a
                                                         // MAP_SHARED memfd), NOT by this address space.
                                                         // Teardown drops the PTE but must NOT free the
                                                         // frame (another process may still map it), and
                                                         // fork ALIASES it (same frame) instead of copying,
                                                         // so both processes see each other's writes.
const uint64_t PAGE_MASK   = 0x000FFFFFFFFFF000ULL;      // bits 12..51: the 4 KiB frame address
const uint64_t FLAG_MASK   = 0xFFF;                      // low 12 control bits (incl. the AVL PTE_PRIV)

inline uint64_t pml4Index(uint64_t va) { return (va >> 39) & 0x1FF; }
inline uint64_t pdptIndex(uint64_t va) { return (va >> 30) & 0x1FF; }
inline uint64_t pdIndex(uint64_t va)   { return (va >> 21) & 0x1FF; }
inline uint64_t ptIndex(uint64_t va)   { return (va >> 12) & 0x1FF; }
inline uint64_t pageOffset(uint64_t va){ return va & 0xFFF; }

// Build a table entry: frame address (masked) + low control flags + the NX bit (63),
// which sits outside the low 12 bits and so is carried separately.
inline uint64_t makeEntry(uint64_t pa, uint64_t flags) {
	return (pa & PAGE_MASK) | (flags & FLAG_MASK) | (flags & PTE_NX);
}
inline uint64_t entryAddr(uint64_t e) { return e & PAGE_MASK; }
inline bool entryPresent(uint64_t e)  { return e & PTE_PRESENT; }

// Canonicalize a 48-bit VA: bits 48..63 become a copy of bit 47. A naively-ported low
// window (bit 47 = 0) is already canonical; this guards future high-half use.
inline uint64_t canonical(uint64_t va) {
	return (va & (1ULL << 47)) ? (va | 0xFFFF000000000000ULL)
	                           : (va & 0x0000FFFFFFFFFFFFULL);
}
inline bool isCanonical(uint64_t va) { return canonical(va) == va; }

}
