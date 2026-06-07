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
}

namespace arch {

void cpuInit() {
	// GDT first: the IDT gates use code selector 0x08, valid only once we own
	// the GDT layout (bootloaders differ). Idt::initialize remaps the PIC and
	// installs all 256 gates (incl. the int 0x80 syscall gate) then sti.
	g_gdt.initialize();
	g_idt.initialize();
	// Legacy PC input: the PS/2 keyboard (IRQ1).
	g_keyboard.initialize();
}

void cpuDisableInterrupts() { __asm__ __volatile__("cli"); }
void cpuEnableInterrupts() { __asm__ __volatile__("sti"); }
void cpuHalt() { __asm__ __volatile__("hlt"); }

}
