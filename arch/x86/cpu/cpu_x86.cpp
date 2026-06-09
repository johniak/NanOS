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

// Boot kernel stack for the very first ring3->ring0 trap (the syscall self-test and
// before the scheduler runs). Once the scheduler is live, each task supplies its own
// kernel stack via arch::setKernelStack(task->esp0) on every switch.
unsigned char g_bootKstack[8192] __attribute__((aligned(16)));
}

namespace arch {

void faultInit();   // arch/x86/cpu/fault_x86.cpp — #GP/#PF debug handlers

void cpuInit() {
	// GDT first: the IDT gates use code selector 0x08, valid only once we own
	// the GDT layout (bootloaders differ). Idt::initialize remaps the PIC and
	// installs all 256 gates (incl. the int 0x80 syscall gate) then sti.
	g_gdt.initialize();
	// Point the TSS at the boot kernel stack and load the task register, so
	// ring3->ring0 traps (int 0x80, IRQs) have a kernel stack to switch to until the
	// scheduler starts repointing TSS.esp0 at each task's own stack.
	g_gdt.setKernelStack((unsigned) (g_bootKstack + sizeof(g_bootKstack)));
	g_gdt.loadTss();
	g_idt.initialize();
	faultInit();
	// Legacy PC input: the PS/2 keyboard (IRQ1).
	g_keyboard.initialize();
}

void cpuDisableInterrupts() { __asm__ __volatile__("cli"); }
void cpuEnableInterrupts() { __asm__ __volatile__("sti"); }
void cpuHalt() { __asm__ __volatile__("hlt"); }

// Repoint TSS.esp0 (where the CPU lands on the next ring3->ring0 trap). The scheduler
// calls this on every switch with the next task's kernel-stack top.
void setKernelStack(unsigned esp0) { g_gdt.setKernelStack(esp0); }

// ---- CPU identification via CPUID (for /proc/cpuinfo) --------------------------------
namespace {
inline void cpuid(unsigned leaf, unsigned* a, unsigned* b, unsigned* c, unsigned* d) {
	__asm__ __volatile__("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
inline void put4(char* dst, unsigned v) {
	dst[0] = (char) v; dst[1] = (char) (v >> 8); dst[2] = (char) (v >> 16); dst[3] = (char) (v >> 24);
}
inline int appendFlag(char* dst, int p, int cap, const char* f) {
	if (p && p < cap - 1) dst[p++] = ' ';
	for (int i = 0; f[i] && p < cap - 1; i++) dst[p++] = f[i];
	dst[p] = 0;
	return p;
}
}

void cpuIdentify(CpuInfo* out) {
	unsigned a, b, c, d;
	cpuid(0, &a, &b, &c, &d);              // leaf 0: max leaf + vendor (EBX, EDX, ECX)
	unsigned maxLeaf = a;
	put4(out->vendor + 0, b);
	put4(out->vendor + 4, d);
	put4(out->vendor + 8, c);
	out->vendor[12] = 0;

	out->family = out->model = out->stepping = 0;
	out->flags[0] = 0;
	unsigned featEdx = 0, featEcx = 0;
	if (maxLeaf >= 1) {                    // leaf 1: family/model/stepping + feature bits
		cpuid(1, &a, &b, &c, &d);
		unsigned baseFamily = (a >> 8) & 0xF, baseModel = (a >> 4) & 0xF;
		out->stepping = a & 0xF;
		out->family = baseFamily + ((baseFamily == 0xF) ? ((a >> 20) & 0xFF) : 0);
		out->model = baseModel + ((baseFamily == 0xF || baseFamily == 0x6) ? (((a >> 16) & 0xF) << 4) : 0);
		featEdx = d;
		featEcx = c;
	}
	int p = 0;
	struct { unsigned bit; const char* name; } edxF[] = {
		{ 0, "fpu" }, { 4, "tsc" }, { 5, "msr" }, { 6, "pae" }, { 9, "apic" },
		{ 15, "cmov" }, { 23, "mmx" }, { 25, "sse" }, { 26, "sse2" },
	};
	for (unsigned i = 0; i < sizeof edxF / sizeof edxF[0]; i++)
		if (featEdx & (1u << edxF[i].bit))
			p = appendFlag(out->flags, p, (int) sizeof out->flags, edxF[i].name);
	if (featEcx & (1u << 0))  p = appendFlag(out->flags, p, (int) sizeof out->flags, "sse3");
	if (featEcx & (1u << 19)) p = appendFlag(out->flags, p, (int) sizeof out->flags, "sse4_1");

	out->brand[0] = 0;                     // extended leaves 0x80000002-4: brand string
	cpuid(0x80000000u, &a, &b, &c, &d);
	if (a >= 0x80000004u) {
		unsigned* w = (unsigned*) out->brand;
		for (unsigned leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
			cpuid(leaf, &a, &b, &c, &d);
			*w++ = a; *w++ = b; *w++ = c; *w++ = d;
		}
		out->brand[48] = 0;
	}
}

}
