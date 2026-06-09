/*
 * arch/cpu.h — MI/MD contract for core CPU control.
 */
#pragma once

namespace arch {

// Bring up the CPU's descriptor tables and interrupt vectors (x86: GDT + IDT +
// PIC). Must be called before enabling interrupts or taking a fault.
void cpuInit();

void cpuDisableInterrupts();
void cpuEnableInterrupts();
void cpuHalt();

// CPU identification for /proc/cpuinfo (x86: CPUID). Machine-independent shape so the MI
// /proc layer can render it; the x86 implementation fills it from CPUID leaves.
struct CpuInfo {
	char vendor[16];   // leaf 0 vendor string (e.g. "GenuineIntel", "AuthenticAMD")
	char brand[52];    // leaves 0x80000002-4 brand string, or "" if unsupported
	unsigned family;   // display family/model/stepping (leaf 1)
	unsigned model;
	unsigned stepping;
	char flags[128];   // space-separated feature flags (fpu, tsc, sse, ...)
};
void cpuIdentify(CpuInfo* out);

}
