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

// Per-depth kernel stacks for ring3->ring0 transitions (TSS.esp0). Each nesting
// level of spawn needs its OWN stack: while a parent is suspended mid-spawn, the
// child's traps must not land on (and clobber) the parent's frames. The boot chain
// init -> shell -> command is already depth 2, so we keep several. Separate from the
// boot stack, which holds the kernel's own execProgram frames.
#define NUM_KSTACKS 6
unsigned char g_kstacks[NUM_KSTACKS][8192];
}

namespace arch {

void faultInit();   // arch/x86/cpu/fault_x86.cpp — #GP/#PF debug handlers

void cpuInit() {
	// GDT first: the IDT gates use code selector 0x08, valid only once we own
	// the GDT layout (bootloaders differ). Idt::initialize remaps the PIC and
	// installs all 256 gates (incl. the int 0x80 syscall gate) then sti.
	g_gdt.initialize();
	// Point the TSS at the depth-0 kernel stack and load the task register, so
	// ring3->ring0 traps (int 0x80, IRQs) have a kernel stack to switch to.
	g_gdt.setKernelStack((unsigned) (g_kstacks[0] + sizeof(g_kstacks[0])));
	g_gdt.loadTss();
	g_idt.initialize();
	faultInit();
	// Legacy PC input: the PS/2 keyboard (IRQ1).
	g_keyboard.initialize();
}

void cpuDisableInterrupts() { __asm__ __volatile__("cli"); }
void cpuEnableInterrupts() { __asm__ __volatile__("sti"); }
void cpuHalt() { __asm__ __volatile__("hlt"); }

// Per-depth kernel stack top for TSS.esp0 (used by the nested-spawn machinery in
// usermode_x86.cpp). Each spawn nesting level gets its own stack so a child's traps
// never clobber a suspended parent's frames. Clamp very deep nesting to the last.
unsigned kstackTop(int depth) {
	if (depth < 0)
		depth = 0;
	if (depth >= NUM_KSTACKS)
		depth = NUM_KSTACKS - 1;
	return (unsigned) (g_kstacks[depth] + sizeof(g_kstacks[depth]));
}

// Repoint TSS.esp0 (where the CPU lands on the next ring3->ring0 trap).
void setKernelStack(unsigned esp0) { g_gdt.setKernelStack(esp0); }

}
