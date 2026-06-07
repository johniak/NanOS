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

}
