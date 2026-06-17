#include "Gdt64.h"
#include <string.h>   // memset

namespace kernel {

// The TSS and its IST1 (double-fault) stack live for the kernel's lifetime; the descriptor
// holds a pointer into g_tss64. File scope (.bss, trivial init) so no global ctor is needed.
static Tss64  g_tss64 __attribute__((aligned(16)));
static uint8_t g_dfStack[4096] __attribute__((aligned(16)));   // IST1: dedicated #DF stack

void Gdt64::setCodeData(GdtEntry64& e, uint8_t access, uint8_t flags) {
    // base/limit are ignored by the CPU in long mode; zero them. access carries
    // present/DPL/type; flags carries the L (long-mode) bit for code segments.
    e.limit_lo = 0; e.base_lo = 0; e.base_mid = 0; e.base_hi = 0;
    e.access = access;
    e.flags = flags;
}

void Gdt64::setTss(uint64_t base, uint32_t limit) {
    TssDescriptor64& t = table.tss;
    memset(&t, 0, sizeof(t));
    t.limit_lo  = (uint16_t) (limit & 0xFFFF);
    t.base_lo   = (uint16_t) (base & 0xFFFF);
    t.base_mid1 = (uint8_t)  ((base >> 16) & 0xFF);
    t.access    = 0x89;                                   // present | type=9 (avail 64-bit TSS)
    t.flags     = (uint8_t)  ((limit >> 16) & 0x0F);      // limit 19:16, G=0
    t.base_mid2 = (uint8_t)  ((base >> 24) & 0xFF);
    t.base_hi   = (uint32_t) ((base >> 32) & 0xFFFFFFFF);
    t.reserved  = 0;
}

void Gdt64::initialize() {
    memset(&table, 0, sizeof(table));

    // Code: present, ring/DPL, exec/read. flags = G | L (0xA0): L=1 marks a 64-bit code
    // segment (D/B must be 0). Data: present, write. Data segments ignore the L bit.
    setCodeData(table.kcode,   0x9A, 0xA0);   // 0x08 ring-0 code64           (SYSCALL CS)
    setCodeData(table.kdata,   0x92, 0x00);   // 0x10 ring-0 data64           (SYSCALL SS)
    // 0x18 user code32 placeholder (DPL3): SYSRET derives the user selectors from this base
    // (STAR[63:48]=0x18) and never loads 0x18 itself in long mode. Built as a present DPL3
    // 32-bit code segment (D=1, not L) per the OSDev "SYSRET" convention. flags 0xCF = G|D
    // + limit 19:16; setCodeData zeroes base/limit, so set the full limit so it is well-formed.
    setCodeData(table.ucode32, 0xFA, 0xCF);   // 0x18 ring-3 code32 placeholder (DPL=3)
    table.ucode32.limit_lo = 0xFFFF;          // full 4 GiB limit (cosmetic; never loaded)
    setCodeData(table.udata,   0xF2, 0x00);   // 0x20 ring-3 data   (DPL=3)   -> SYSRET SS 0x23
    setCodeData(table.ucode,   0xFA, 0xA0);   // 0x28 ring-3 code64 (L=1,DPL3)-> SYSRET CS 0x2B

    // TSS @ 0x30. Point IST1 at a dedicated stack so #DF always lands on solid ground; mark
    // "no I/O bitmap". rsp0 is filled by setKernelStack before the first ring3->ring0 trap.
    g_tss64.ist1 = (uint64_t) (g_dfStack + sizeof(g_dfStack));
    g_tss64.iomap_base = sizeof(Tss64);
    setTss((uint64_t) &g_tss64, sizeof(Tss64) - 1);

    ptr.limit = sizeof(table) - 1;
    ptr.base  = (uint64_t) &table;

    // lgdt, then reload CS via a far return (long mode has no far-jmp-to-immediate from C):
    // push the new code selector + a RIP, lretq pops both. Then reload the data segment regs
    // with the kernel data selector (their bases are forced to 0 in long mode regardless).
    __asm__ __volatile__(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        : : "m"(ptr) : "rax", "memory");
}

void Gdt64::setKernelStack(uint64_t rsp0) { g_tss64.rsp0 = rsp0; }

void Gdt64::loadTss() { __asm__ __volatile__("ltr %0" : : "r"((uint16_t) 0x30)); }

}  // namespace kernel
