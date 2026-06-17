/*
 * Interrupt64.h — x86-64 trap frame + interrupt handler registry.
 *
 * Registers MUST match the stack layout built by isr64.S/irq64.S exactly: the common stub
 * saves the 15 GPRs in an order such that the LAST-pushed register (r15) is at the LOWEST
 * address, i.e. the field declared FIRST here. Below the GPRs sit int_no/err_code (pushed by
 * the per-vector entry / CPU) and then the CPU's iretq frame (rip/cs/rflags/rsp/ss). The
 * struct is named Registers to mirror i686 so MI's opaque arch::TrapFrame stays portable.
 */
#pragma once
#include <stdint.h>

namespace kernel {

#define IRQ0 32
#define IRQ1 33
#define IRQ2 34
#define IRQ3 35
#define IRQ4 36
#define IRQ5 37
#define IRQ6 38
#define IRQ7 39
#define IRQ8 40
#define IRQ9 41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

struct Registers {
    // 15 GPRs saved by the stub (r15 pushed last -> lowest address -> declared first).
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    // Pushed by the per-vector entry (int_no) and CPU/stub (err_code).
    uint64_t int_no, err_code;
    // Pushed automatically by the CPU on the interrupt (rsp/ss always pushed in long mode).
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*IsrHandler)(Registers*);

class Interrupt {
public:
    static void registerInterruptHandler(unsigned char n, IsrHandler handler);
};

}  // namespace kernel
