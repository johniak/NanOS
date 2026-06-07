/*
 * cpu_x86.cpp — x86 implementation of <arch/cpu.h>.
 */
#include <arch/cpu.h>
#include "Gdt.h"
#include "Idt.h"
#include "Keyboard.h"

namespace {
// The GDT/IDT live for the kernel's lifetime: the CPU registers point at the
// descriptor tables held inside these objects. File-scope (.bss, trivial ctor)
// so no global constructor is required.
kernel::Gdt g_gdt;
kernel::Idt g_idt;
kernel::Keyboard g_keyboard;

// Dedicated kernel stack for ring3->ring0 transitions (TSS.esp0). Must be
// SEPARATE from the boot stack, which holds the live execProgram frames and the
// longjmp target that exit() returns to.
unsigned char g_userKstack[8192];
}

namespace arch {

void faultInit();   // arch/x86/cpu/fault_x86.cpp — #GP/#PF debug handlers

void cpuInit() {
	// GDT first: the IDT gates use code selector 0x08, valid only once we own
	// the GDT layout (bootloaders differ). Idt::initialize remaps the PIC and
	// installs all 256 gates (incl. the int 0x80 syscall gate) then sti.
	g_gdt.initialize();
	// Point the TSS at the dedicated kernel stack and load the task register, so
	// ring3->ring0 traps (int 0x80, IRQs) have a kernel stack to switch to.
	g_gdt.setKernelStack((unsigned) (g_userKstack + sizeof(g_userKstack)));
	g_gdt.loadTss();
	g_idt.initialize();
	faultInit();
	// Legacy PC input: the PS/2 keyboard (IRQ1).
	g_keyboard.initialize();
}

void cpuDisableInterrupts() { __asm__ __volatile__("cli"); }
void cpuEnableInterrupts() { __asm__ __volatile__("sti"); }
void cpuHalt() { __asm__ __volatile__("hlt"); }

}
