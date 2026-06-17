/*
 * Tss64.h — x86-64 Task State Segment (104 bytes).
 *
 * Long mode drops the 32-bit TSS's saved GPRs/segments and ss0. The CPU uses only:
 *   - rsp0/1/2: the stack to switch to on an interrupt that raises CPL (ring3->ring0),
 *   - ist1..ist7: the Interrupt Stack Table — gates with a non-zero IST index land on a
 *     known-good stack regardless of the interrupted rsp (we use ist1 for #DF),
 *   - iomap_base: offset to the I/O permission bitmap; == sizeof(Tss64) means "none".
 * All fields are at architecture-fixed offsets (rsp0 @ 4); the struct is packed so the
 * compiler can't insert padding that would shift them.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct Tss64 {
    uint32_t reserved0;                                  // 0
    uint64_t rsp0;                                       // 4   kernel stack for ring3->ring0
    uint64_t rsp1;                                       // 12
    uint64_t rsp2;                                       // 20
    uint64_t reserved1;                                  // 28
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;  // 36..91  Interrupt Stack Table
    uint64_t reserved2;                                  // 92
    uint16_t reserved3;                                  // 100
    uint16_t iomap_base;                                 // 102 (== sizeof(Tss64) => no bitmap)
} __attribute__((packed));

}  // namespace kernel
