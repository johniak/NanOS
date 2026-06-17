/*
 * Gdt64.h — x86-64 Global Descriptor Table.
 *
 * Code/data descriptors keep the legacy 8-byte shape, but in long mode the base and limit
 * are ignored — only the access byte and the L (long-mode) flag matter. The TSS descriptor
 * is the one that grows: a 16-byte system descriptor carrying a full 64-bit base.
 *
 * Selectors (must match the IDT gates and the CS reload in initialize):
 *   0x08 kernel code64, 0x10 kernel data64, 0x18 user code64 (DPL3),
 *   0x20 user data64 (DPL3), 0x28 TSS (16-byte descriptor, indices 5..6).
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
        GdtEntry64      kcode;     // 0x08
        GdtEntry64      kdata;     // 0x10
        GdtEntry64      ucode;     // 0x18 (DPL3, used from Plan 6)
        GdtEntry64      udata;     // 0x20 (DPL3, used from Plan 6)
        TssDescriptor64 tss;       // 0x28 (16 bytes, occupies two 8-byte slots)
    } table;
    Gdt64Ptr ptr;
public:
    void initialize();                  // build table, lgdt, reload CS/segs, point TSS at IST1
    void setKernelStack(uint64_t rsp0); // TSS.rsp0 — kernel stack for ring3->ring0 (Plan 6)
    void loadTss();                     // ltr 0x28 (after initialize)
private:
    static void setCodeData(GdtEntry64& e, uint8_t access, uint8_t flags);
    void setTss(uint64_t base, uint32_t limit);
};

}  // namespace kernel
