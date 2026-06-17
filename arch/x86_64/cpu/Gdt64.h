/*
 * Gdt64.h — x86-64 Global Descriptor Table.
 *
 * Code/data descriptors keep the legacy 8-byte shape, but in long mode the base and limit
 * are ignored — only the access byte and the L (long-mode) flag matter. The TSS descriptor
 * is the one that grows: a 16-byte system descriptor carrying a full 64-bit base.
 *
 * Selectors (must match the IDT gates, the CS reload in initialize, AND the SYSCALL/SYSRET
 * MSRs in syscall_x86_64.cpp). The order is dictated by SYSRET, which derives BOTH user
 * selectors from a single base STAR[63:48] = 0x18:
 *   - SYSCALL: CS = STAR[47:32] = 0x08,  SS = 0x08+8  = 0x10  (kernel code64 / data64)
 *   - SYSRET : CS = STAR[63:48]+16 | 3 = 0x28|3 = 0x2B (user code64),
 *              SS = STAR[63:48]+8  | 3 = 0x20|3 = 0x23 (user data)
 * Hence the layout MUST be:
 *   0x08 kernel code64 (DPL0)            <- SYSCALL CS
 *   0x10 kernel data64 (DPL0)            <- SYSCALL SS
 *   0x18 user code32 placeholder (DPL3)  <- SYSRET base (never loaded in long mode)
 *   0x20 user data    (DPL3)             <- SYSRET SS (0x23)
 *   0x28 user code64  (L=1, DPL3)        <- SYSRET CS (0x2B)
 *   0x30 TSS (16-byte descriptor, occupies indices 6..7).
 */
#pragma once
#include "Tss64.h"
#include <stdint.h>

namespace kernel {

// 8-byte code/data descriptor. flags high nibble = G/D/L/AVL; low nibble = limit 19:16.
struct GdtEntry64 {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags;
    uint8_t  base_hi;
} __attribute__((packed));

// 16-byte TSS system descriptor: the base is scattered across 64 bits.
struct TssDescriptor64 {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid1;
    uint8_t  access;        // 0x89 = present, DPL0, type 9 (available 64-bit TSS)
    uint8_t  flags;         // limit 19:16 + granularity (0 here; TSS is small)
    uint8_t  base_mid2;
    uint32_t base_hi;       // base bits 63:32
    uint32_t reserved;
} __attribute__((packed));

struct Gdt64Ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

class Gdt64 {
    struct __attribute__((packed)) Table {
        GdtEntry64      null_;     // 0x00
        GdtEntry64      kcode;     // 0x08 kernel code64 (DPL0) — SYSCALL CS
        GdtEntry64      kdata;     // 0x10 kernel data64 (DPL0) — SYSCALL SS
        GdtEntry64      ucode32;   // 0x18 user code32 placeholder (DPL3) — SYSRET base
        GdtEntry64      udata;     // 0x20 user data (DPL3) — SYSRET SS (0x23)
        GdtEntry64      ucode;     // 0x28 user code64 (L=1, DPL3) — SYSRET CS (0x2B)
        TssDescriptor64 tss;       // 0x30 (16 bytes, occupies indices 6..7)
    } table;
    Gdt64Ptr ptr;
public:
    void initialize();                  // build table, lgdt, reload CS/segs, point TSS at IST1
    void setKernelStack(uint64_t rsp0); // TSS.rsp0 — kernel stack for ring3->ring0 (Plan 6)
    void loadTss();                     // ltr 0x30 (after initialize)
private:
    static void setCodeData(GdtEntry64& e, uint8_t access, uint8_t flags);
    void setTss(uint64_t base, uint32_t limit);
};

}  // namespace kernel
