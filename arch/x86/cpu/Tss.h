/*
 * Tss.h — i386 Task State Segment (32-bit, 104 bytes).
 *
 * We only use ss0/esp0: on a ring3->ring0 transition (int 0x80, IRQ) the CPU
 * loads esp0/ss0 from the active TSS to switch to a kernel stack. iomap_base =
 * sizeof(Tss) means "no I/O permission bitmap". Layout offsets are fixed by the
 * architecture: esp0 @4, ss0 @8, iomap_base @102.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct Tss {
	uint32_t prev_tss;                                  // 0
	uint32_t esp0;                                      // 4  kernel stack ptr
	uint32_t ss0;                                       // 8  kernel stack segment
	uint32_t esp1, ss1, esp2, ss2;                      // 12..27
	uint32_t cr3, eip, eflags, eax, ecx, edx, ebx;      // 28..55
	uint32_t esp, ebp, esi, edi;                        // 56..71
	uint32_t es, cs, ss, ds, fs, gs, ldt;               // 72..99
	uint16_t trap;                                      // 100
	uint16_t iomap_base;                                // 102
} __attribute__((packed));

}
