/*
 * cpu_x86_64.cpp — x86-64 implementation of <arch/cpu.h>.
 *
 * Plan 4 scope: descriptor-table + interrupt-vector bring-up and the interrupt-flag
 * primitives. powerOff/cpuIdentify/rtcEpoch (CPUID/ACPI/CMOS — port semantics identical to
 * i686) are ported in a later plan when MI code that calls them is staged in; archLoadThreadTls
 * is a no-op until %fs.base / TLS arrives in Plan 6.
 */
#include <arch/cpu.h>
#include "Gdt64.h"
#include "Idt64.h"

namespace {
// GDT/IDT live for the kernel's lifetime (the CPU registers point into them). File scope
// (.bss, trivial ctor) so no global constructor is required (the loader calls kmain directly).
kernel::Gdt64 g_gdt;
kernel::Idt64 g_idt;

// Boot kernel stack for the first ring3->ring0 trap (before any scheduler). Once tasks exist
// they supply their own kernel stack via TSS.rsp0 on every switch.
unsigned char g_bootKstack[8192] __attribute__((aligned(16)));
}

namespace arch {

void faultInit();   // arch/x86_64/cpu/fault_x86_64.cpp — #GP/#PF debug handlers

void cpuInit() {
    // GDT first: the IDT gates reference code selector 0x08, valid only once we own the GDT.
    g_gdt.initialize();
    // Point TSS.rsp0 at the boot kernel stack and load the task register, so future
    // ring3->ring0 traps have a kernel stack to land on.
    g_gdt.setKernelStack((uint64_t) (g_bootKstack + sizeof(g_bootKstack)));
    g_gdt.loadTss();
    // IDT: remap the PIC and install all 256 gates (CPU exceptions + IRQs). No sti yet.
    g_idt.initialize();
    faultInit();
}

// Repoint TSS.rsp0 (the kernel stack the CPU loads on a ring3->ring0 interrupt/exception gate).
// The scheduler calls this via setKernelStack (sched_x86_64.cpp) on every task switch; g_gdt is
// file-scoped here, so this thin setter is the way other arch TUs reach it.
void cpuSetTssKernelStack(uint64_t rsp0) { g_gdt.setKernelStack(rsp0); }

void cpuDisableInterrupts() { __asm__ __volatile__("cli"); }
void cpuEnableInterrupts()  { __asm__ __volatile__("sti"); }
void cpuHalt()              { __asm__ __volatile__("hlt"); }

// Save RFLAGS then disable interrupts; restore (re-enabling IF only if it had been set), so a
// critical section nests correctly regardless of the caller's interrupt state.
unsigned long cpuIrqSave() {
    unsigned long flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
void cpuIrqRestore(unsigned long flags) {
    __asm__ __volatile__("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

// TLS on x86-64 uses %fs.base, set up for user threads in Plan 6. No-op until then.
void archLoadThreadTls(unsigned /*base*/) { }

}  // namespace arch
